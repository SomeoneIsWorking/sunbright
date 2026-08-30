#include <sunbright/native_render/semantic_2d_pass.h>

#include "../shaders/picture_frag_spv.h"
#include "../shaders/picture_vert_spv.h"
#include "../shaders/solid_rectangle_frag_spv.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <limits>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sb::native_render {
namespace {

constexpr SDL_GPUTextureFormat kImageFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
constexpr auto kFenceTimeout = std::chrono::seconds(5);

struct GpuVertex {
    float position[2];
    float uv[2];
    float color[4];
};

struct CanvasUniform {
    float origin[2];
    float extent[2];
};

struct PictureStyleUniform {
    float black[4];
    float white[4];
    float colorMix[4];
    float alphaMix[4];
    std::int32_t control[4];
    float opacity[4];
};

static_assert(sizeof(GpuVertex) == 32);
static_assert(sizeof(CanvasUniform) == 16);
static_assert(sizeof(PictureStyleUniform) == 96);

struct ImageKey {
    std::uint64_t resource = 0;
    std::uint64_t revision = 0;

    bool operator==(const ImageKey&) const = default;
};

struct ImageKeyHash {
    std::size_t operator()(ImageKey key) const noexcept {
        return std::hash<std::uint64_t>{}(key.resource) ^
               (std::hash<std::uint64_t>{}(key.revision) << 1U);
    }
};

struct SamplerKey {
    AddressMode addressU = AddressMode::Clamp;
    AddressMode addressV = AddressMode::Clamp;
    FilterMode minFilter = FilterMode::Nearest;
    FilterMode magFilter = FilterMode::Nearest;
    MipFilter mipFilter = MipFilter::None;

    bool operator==(const SamplerKey&) const = default;
};

struct SamplerKeyHash {
    std::size_t operator()(SamplerKey key) const noexcept {
        return static_cast<std::size_t>(key.addressU) |
               (static_cast<std::size_t>(key.addressV) << 2U) |
               (static_cast<std::size_t>(key.minFilter) << 4U) |
               (static_cast<std::size_t>(key.magFilter) << 5U) |
               (static_cast<std::size_t>(key.mipFilter) << 6U);
    }
};

struct CachedImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    SDL_GPUTexture* texture = nullptr;
    SDL_GPUTransferBuffer* upload = nullptr;
};

struct VertexStorage {
    SDL_GPUBuffer* buffer = nullptr;
    SDL_GPUTransferBuffer* upload = nullptr;
    std::size_t capacity = 0;
};

SDL_GPUShader* make_shader(SDL_GPUDevice* device, const void* code, std::size_t bytes,
                           SDL_GPUShaderStage stage, Uint32 samplers,
                           Uint32 uniformBuffers) noexcept {
    SDL_GPUShaderCreateInfo info{};
    info.code = static_cast<const Uint8*>(code);
    info.code_size = bytes;
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = stage;
    info.num_samplers = samplers;
    info.num_uniform_buffers = uniformBuffers;
    return SDL_CreateGPUShader(device, &info);
}

void assign(float (&destination)[4], Color source) noexcept {
    destination[0] = source.r;
    destination[1] = source.g;
    destination[2] = source.b;
    destination[3] = source.a;
}

SDL_GPUFilter filter(FilterMode value) noexcept {
    return value == FilterMode::Linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
}

SDL_GPUSamplerMipmapMode mip_filter(MipFilter value) noexcept {
    return value == MipFilter::Linear ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR
                                      : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
}

SDL_GPUSamplerAddressMode address_mode(AddressMode value) noexcept {
    switch (value) {
    case AddressMode::Repeat:
        return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    case AddressMode::Mirror:
        return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    case AddressMode::Clamp:
        return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    }
    return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
}

SamplerKey sampler_key(const PictureTexture& texture) noexcept {
    return {texture.addressU, texture.addressV, texture.minFilter, texture.magFilter,
            texture.mipFilter};
}

bool wait_for_fence(SDL_GPUDevice* device, SDL_GPUFence* fence) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + kFenceTimeout;
    while (!SDL_QueryGPUFence(device, fence)) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

bool multiplication_fits(std::uint32_t width, std::uint32_t height, std::size_t& bytes) noexcept {
    const std::uint64_t total = static_cast<std::uint64_t>(width) * height * 4U;
    if (width == 0 || height == 0 || total > std::numeric_limits<Uint32>::max())
        return false;
    bytes = static_cast<std::size_t>(total);
    return true;
}

std::size_t next_capacity(std::size_t required) noexcept {
    std::size_t capacity = 256;
    while (capacity < required && capacity <= std::numeric_limits<Uint32>::max() / 2U)
        capacity *= 2U;
    return std::max(capacity, required);
}

} // namespace

struct Semantic2dPassImpl {
    explicit Semantic2dPassImpl(SDL_GPUDevice* value) noexcept : device(value) {}

    SDL_GPUDevice* device = nullptr;
    SDL_GPUShader* vertexShader = nullptr;
    SDL_GPUShader* pictureFragmentShader = nullptr;
    SDL_GPUShader* solidFragmentShader = nullptr;
    std::unordered_map<SDL_GPUTextureFormat, SDL_GPUGraphicsPipeline*> picturePipelines{};
    std::unordered_map<SDL_GPUTextureFormat, SDL_GPUGraphicsPipeline*> solidPipelines{};
    std::unordered_map<ImageKey, CachedImage, ImageKeyHash> images{};
    std::unordered_map<ImageKey, CachedImage, ImageKeyHash> pendingImages{};
    std::vector<ImageKey> currentImages{};
    std::unordered_map<SamplerKey, SDL_GPUSampler*, SamplerKeyHash> samplers{};
    VertexStorage vertices{};
    std::vector<VertexStorage> retiredVertexStorage{};
    bool encodeActive = false;
    bool encodeSucceeded = false;
};

namespace {

void release_vertex_storage(SDL_GPUDevice* device, VertexStorage storage) noexcept {
    if (storage.upload != nullptr)
        SDL_ReleaseGPUTransferBuffer(device, storage.upload);
    if (storage.buffer != nullptr)
        SDL_ReleaseGPUBuffer(device, storage.buffer);
}

void release_image(SDL_GPUDevice* device, CachedImage image) noexcept {
    if (image.upload != nullptr)
        SDL_ReleaseGPUTransferBuffer(device, image.upload);
    if (image.texture != nullptr)
        SDL_ReleaseGPUTexture(device, image.texture);
}

const CachedImage* find_image(const Semantic2dPassImpl& impl, ImageKey key) noexcept {
    if (const auto resident = impl.images.find(key); resident != impl.images.end())
        return &resident->second;
    if (const auto pending = impl.pendingImages.find(key); pending != impl.pendingImages.end())
        return &pending->second;
    return nullptr;
}

SDL_GPUGraphicsPipeline*
ensure_pipeline(Semantic2dPassImpl& impl,
                std::unordered_map<SDL_GPUTextureFormat, SDL_GPUGraphicsPipeline*>& pipelines,
                SDL_GPUShader* fragmentShader, SDL_GPUTextureFormat format, const char* label,
                std::string& error) {
    const auto existing = pipelines.find(format);
    if (existing != pipelines.end())
        return existing->second;
    if (format == SDL_GPU_TEXTUREFORMAT_INVALID ||
        !SDL_GPUTextureSupportsFormat(impl.device, format, SDL_GPU_TEXTURETYPE_2D,
                                      SDL_GPU_TEXTUREUSAGE_COLOR_TARGET)) {
        error = "semantic 2D target format is not a supported SDL GPU color target";
        return nullptr;
    }

    SDL_GPUVertexBufferDescription vertexBuffer{};
    vertexBuffer.slot = 0;
    vertexBuffer.pitch = sizeof(GpuVertex);
    vertexBuffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    const std::array<SDL_GPUVertexAttribute, 3> attributes{
        SDL_GPUVertexAttribute{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,
                               offsetof(GpuVertex, position)},
        SDL_GPUVertexAttribute{1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GpuVertex, uv)},
        SDL_GPUVertexAttribute{2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
                               offsetof(GpuVertex, color)},
    };

    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = format;
    colorTarget.blend_state.enable_blend = true;
    colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    colorTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    colorTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    colorTarget.blend_state.color_write_mask = static_cast<SDL_GPUColorComponentFlags>(
        SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B |
        SDL_GPU_COLORCOMPONENT_A);
    colorTarget.blend_state.enable_color_write_mask = true;

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = impl.vertexShader;
    info.fragment_shader = fragmentShader;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.vertex_input_state.vertex_buffer_descriptions = &vertexBuffer;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attributes.data();
    info.vertex_input_state.num_vertex_attributes = attributes.size();
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.target_info.color_target_descriptions = &colorTarget;
    info.target_info.num_color_targets = 1;
    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(impl.device, &info);
    if (pipeline == nullptr) {
        error = std::string(label) + " pipeline creation failed: " + SDL_GetError();
        return nullptr;
    }
    pipelines.emplace(format, pipeline);
    return pipeline;
}

bool ensure_vertex_storage(Semantic2dPassImpl& impl, std::size_t required, std::string& error) {
    if (required == 0 || required <= impl.vertices.capacity)
        return true;
    const std::size_t capacity = next_capacity(required);
    if (capacity > std::numeric_limits<Uint32>::max()) {
        error = "semantic 2D vertex upload exceeds SDL GPU limits";
        return false;
    }
    const SDL_GPUBufferCreateInfo bufferInfo{SDL_GPU_BUFFERUSAGE_VERTEX,
                                             static_cast<Uint32>(capacity), 0};
    const SDL_GPUTransferBufferCreateInfo uploadInfo{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                                     static_cast<Uint32>(capacity), 0};
    VertexStorage replacement{SDL_CreateGPUBuffer(impl.device, &bufferInfo),
                              SDL_CreateGPUTransferBuffer(impl.device, &uploadInfo), capacity};
    if (replacement.buffer == nullptr || replacement.upload == nullptr) {
        release_vertex_storage(impl.device, replacement);
        error = std::string("semantic 2D vertex resource allocation failed: ") + SDL_GetError();
        return false;
    }
    if (impl.vertices.buffer != nullptr)
        impl.retiredVertexStorage.push_back(impl.vertices);
    impl.vertices = replacement;
    return true;
}

SDL_GPUSampler* ensure_sampler(Semantic2dPassImpl& impl, const PictureTexture& texture,
                               std::string& error) {
    const SamplerKey key = sampler_key(texture);
    const auto existing = impl.samplers.find(key);
    if (existing != impl.samplers.end())
        return existing->second;
    SDL_GPUSamplerCreateInfo info{};
    info.min_filter = filter(texture.minFilter);
    info.mag_filter = filter(texture.magFilter);
    info.mipmap_mode = mip_filter(texture.mipFilter);
    info.address_mode_u = address_mode(texture.addressU);
    info.address_mode_v = address_mode(texture.addressV);
    info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    info.max_lod = 0.0f;
    SDL_GPUSampler* sampler = SDL_CreateGPUSampler(impl.device, &info);
    if (sampler == nullptr) {
        error = std::string("picture sampler creation failed: ") + SDL_GetError();
        return nullptr;
    }
    impl.samplers.emplace(key, sampler);
    return sampler;
}

} // namespace

Semantic2dPass::Semantic2dPass(SDL_GPUDevice* device) : impl_(new Semantic2dPassImpl(device)) {}

Semantic2dPass::~Semantic2dPass() {
    if (impl_ != nullptr && impl_->encodeActive)
        std::terminate();
    if (impl_ != nullptr && impl_->device != nullptr) {
        for (const auto& [key, sampler] : impl_->samplers) {
            (void)key;
            SDL_ReleaseGPUSampler(impl_->device, sampler);
        }
        for (const auto& [key, image] : impl_->images) {
            (void)key;
            release_image(impl_->device, image);
        }
        for (const auto& [key, image] : impl_->pendingImages) {
            (void)key;
            release_image(impl_->device, image);
        }
        release_vertex_storage(impl_->device, impl_->vertices);
        for (VertexStorage storage : impl_->retiredVertexStorage)
            release_vertex_storage(impl_->device, storage);
        for (const auto& [format, pipeline] : impl_->picturePipelines) {
            (void)format;
            SDL_ReleaseGPUGraphicsPipeline(impl_->device, pipeline);
        }
        for (const auto& [format, pipeline] : impl_->solidPipelines) {
            (void)format;
            SDL_ReleaseGPUGraphicsPipeline(impl_->device, pipeline);
        }
        if (impl_->solidFragmentShader != nullptr)
            SDL_ReleaseGPUShader(impl_->device, impl_->solidFragmentShader);
        if (impl_->pictureFragmentShader != nullptr)
            SDL_ReleaseGPUShader(impl_->device, impl_->pictureFragmentShader);
        if (impl_->vertexShader != nullptr)
            SDL_ReleaseGPUShader(impl_->device, impl_->vertexShader);
    }
    delete impl_;
}

bool Semantic2dPass::initialize(std::string& error) {
    error.clear();
    if (impl_ == nullptr || impl_->device == nullptr) {
        error = "semantic 2D pass requires a host-owned SDL GPU device";
        return false;
    }
    if (impl_->vertexShader != nullptr && impl_->pictureFragmentShader != nullptr &&
        impl_->solidFragmentShader != nullptr)
        return true;
    impl_->vertexShader = make_shader(impl_->device, kPictureVertSpv, sizeof(kPictureVertSpv),
                                      SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    impl_->pictureFragmentShader =
        make_shader(impl_->device, kPictureFragSpv, sizeof(kPictureFragSpv),
                    SDL_GPU_SHADERSTAGE_FRAGMENT, 4, 1);
    impl_->solidFragmentShader =
        make_shader(impl_->device, kSolidRectangleFragSpv, sizeof(kSolidRectangleFragSpv),
                    SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
    if (impl_->vertexShader == nullptr || impl_->pictureFragmentShader == nullptr ||
        impl_->solidFragmentShader == nullptr) {
        error = std::string("semantic 2D shader creation failed: ") + SDL_GetError();
        if (impl_->solidFragmentShader != nullptr)
            SDL_ReleaseGPUShader(impl_->device, impl_->solidFragmentShader);
        if (impl_->pictureFragmentShader != nullptr)
            SDL_ReleaseGPUShader(impl_->device, impl_->pictureFragmentShader);
        if (impl_->vertexShader != nullptr)
            SDL_ReleaseGPUShader(impl_->device, impl_->vertexShader);
        impl_->solidFragmentShader = nullptr;
        impl_->pictureFragmentShader = nullptr;
        impl_->vertexShader = nullptr;
        return false;
    }
    return true;
}

bool Semantic2dPass::encode(const SemanticFrame& frame, const Semantic2dPassTarget& target,
                            std::string& error) {
    error.clear();
    if (impl_ == nullptr || impl_->encodeActive) {
        error = "previous semantic 2D encode has not been completed";
        return false;
    }
    impl_->encodeActive = true;
    impl_->encodeSucceeded = false;
    impl_->pendingImages.clear();
    impl_->currentImages.clear();
    if (!initialize(error))
        return false;
    if (target.commandBuffer == nullptr || target.colorTexture == nullptr) {
        error = "semantic 2D encode requires borrowed command-buffer and color-target handles";
        return false;
    }
    if (frame.targetWidth == 0 || frame.targetHeight == 0) {
        error = "semantic frame has an invalid target extent";
        return false;
    }
    SDL_GPUGraphicsPipeline* picturePipeline =
        ensure_pipeline(*impl_, impl_->picturePipelines, impl_->pictureFragmentShader,
                        target.colorFormat, "picture", error);
    SDL_GPUGraphicsPipeline* solidPipeline =
        ensure_pipeline(*impl_, impl_->solidPipelines, impl_->solidFragmentShader,
                        target.colorFormat, "solid rectangle", error);
    if (picturePipeline == nullptr || solidPipeline == nullptr)
        return false;

    std::unordered_map<ImageKey, const DecodedImageView*, ImageKeyHash> sources;
    impl_->currentImages.reserve(frame.images.size());
    for (const DecodedImageView& image : frame.images) {
        std::size_t bytes = 0;
        const ImageKey key{image.resource, image.revision};
        if (image.resource == 0 || !multiplication_fits(image.width, image.height, bytes) ||
            image.rgba8.size() != bytes || !sources.emplace(key, &image).second) {
            error = "semantic frame contains an invalid or duplicate decoded image";
            return false;
        }
        impl_->currentImages.push_back(key);
        const auto cached = impl_->images.find(key);
        if (cached != impl_->images.end() &&
            (cached->second.width != image.width || cached->second.height != image.height)) {
            error = "picture image revision changed dimensions without changing its semantic key";
            return false;
        }
    }

    std::vector<GpuVertex> vertices;
    vertices.reserve(frame.draws.size() * 6U);
    const auto appendMesh = [&vertices](const auto& mesh) {
        for (const SemanticVertex& source : mesh) {
            vertices.push_back({{source.position.x, source.position.y},
                                {source.uv.x, source.uv.y},
                                {source.color.r, source.color.g, source.color.b, source.color.a}});
        }
    };
    for (const SemanticDraw& semanticDraw : frame.draws) {
        PixelRect viewport{};
        const Canvas& drawCanvas = canvas(semanticDraw);
        if (!valid(semanticDraw) ||
            !resolve_scissor(drawCanvas, {}, frame.targetWidth, frame.targetHeight, viewport)) {
            error = "semantic frame contains an invalid draw context or command";
            return false;
        }
        if (const auto* picture = std::get_if<PictureDraw>(&semanticDraw)) {
            appendMesh(make_mesh(picture->picture));
            for (std::size_t index = 0; index < picture->picture.material.textureCount; ++index) {
                const PictureTexture& texture = picture->picture.material.textures[index];
                const ImageKey key{texture.resource, texture.revision};
                const auto source = sources.find(key);
                const auto cached = impl_->images.find(key);
                const std::uint32_t width = source != sources.end()         ? source->second->width
                                            : cached != impl_->images.end() ? cached->second.width
                                                                            : 0;
                const std::uint32_t height = source != sources.end() ? source->second->height
                                             : cached != impl_->images.end() ? cached->second.height
                                                                             : 0;
                if (width != texture.width || height != texture.height) {
                    error = "picture command references an absent or mismatched decoded image";
                    return false;
                }
                if (std::find(impl_->currentImages.begin(), impl_->currentImages.end(), key) ==
                    impl_->currentImages.end()) {
                    impl_->currentImages.push_back(key);
                }
            }
        } else {
            appendMesh(make_mesh(std::get<SolidRectangleDraw>(semanticDraw).rectangle));
        }
    }

    const std::size_t vertexBytes = vertices.size() * sizeof(GpuVertex);
    if (!ensure_vertex_storage(*impl_, vertexBytes, error))
        return false;
    if (vertexBytes != 0) {
        void* mapped = SDL_MapGPUTransferBuffer(impl_->device, impl_->vertices.upload, true);
        if (mapped == nullptr) {
            error = std::string("semantic 2D vertex upload map failed: ") + SDL_GetError();
            return false;
        }
        std::memcpy(mapped, vertices.data(), vertexBytes);
        SDL_UnmapGPUTransferBuffer(impl_->device, impl_->vertices.upload);
    }

    for (const auto& [key, source] : sources) {
        if (impl_->images.contains(key))
            continue;
        std::size_t bytes = 0;
        (void)multiplication_fits(source->width, source->height, bytes);
        SDL_GPUTextureCreateInfo textureInfo{};
        textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
        textureInfo.format = kImageFormat;
        textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        textureInfo.width = source->width;
        textureInfo.height = source->height;
        textureInfo.layer_count_or_depth = 1;
        textureInfo.num_levels = 1;
        textureInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
        const SDL_GPUTransferBufferCreateInfo uploadInfo{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                                         static_cast<Uint32>(bytes), 0};
        CachedImage image{source->width, source->height,
                          SDL_CreateGPUTexture(impl_->device, &textureInfo),
                          SDL_CreateGPUTransferBuffer(impl_->device, &uploadInfo)};
        if (image.texture == nullptr || image.upload == nullptr) {
            if (image.upload != nullptr)
                SDL_ReleaseGPUTransferBuffer(impl_->device, image.upload);
            if (image.texture != nullptr)
                SDL_ReleaseGPUTexture(impl_->device, image.texture);
            error = std::string("picture image resource allocation failed: ") + SDL_GetError();
            return false;
        }
        void* mapped = SDL_MapGPUTransferBuffer(impl_->device, image.upload, false);
        if (mapped == nullptr) {
            SDL_ReleaseGPUTransferBuffer(impl_->device, image.upload);
            SDL_ReleaseGPUTexture(impl_->device, image.texture);
            error = std::string("picture image upload map failed: ") + SDL_GetError();
            return false;
        }
        std::memcpy(mapped, source->rgba8.data(), bytes);
        SDL_UnmapGPUTransferBuffer(impl_->device, image.upload);
        impl_->pendingImages.emplace(key, image);
    }

    std::vector<std::array<SDL_GPUTextureSamplerBinding, 4>> drawBindings;
    drawBindings.reserve(frame.draws.size());
    for (const SemanticDraw& semanticDraw : frame.draws) {
        std::array<SDL_GPUTextureSamplerBinding, 4> bindings{};
        if (const auto* picture = std::get_if<PictureDraw>(&semanticDraw)) {
            for (std::size_t index = 0; index < bindings.size(); ++index) {
                const PictureTexture& texture =
                    picture->picture.material.textures[std::min<std::size_t>(
                        index, picture->picture.material.textureCount - 1U)];
                SDL_GPUSampler* sampler = ensure_sampler(*impl_, texture, error);
                if (sampler == nullptr)
                    return false;
                const CachedImage* image = find_image(*impl_, {texture.resource, texture.revision});
                if (image == nullptr) {
                    error = "picture image cache lost a validated semantic resource";
                    return false;
                }
                bindings[index] = {image->texture, sampler};
            }
        }
        drawBindings.push_back(bindings);
    }

    if (vertexBytes != 0 || !impl_->pendingImages.empty()) {
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(target.commandBuffer);
        if (copy == nullptr) {
            error = std::string("semantic 2D upload pass creation failed: ") + SDL_GetError();
            return false;
        }
        if (vertexBytes != 0) {
            const SDL_GPUTransferBufferLocation source{impl_->vertices.upload, 0};
            const SDL_GPUBufferRegion destination{impl_->vertices.buffer, 0,
                                                  static_cast<Uint32>(vertexBytes)};
            SDL_UploadToGPUBuffer(copy, &source, &destination, true);
        }
        for (const auto& [key, image] : impl_->pendingImages) {
            (void)key;
            const SDL_GPUTextureTransferInfo source{image.upload, 0, image.width, image.height};
            const SDL_GPUTextureRegion destination{image.texture, 0, 0, 0, 0, 0, image.width,
                                                   image.height,  1};
            SDL_UploadToGPUTexture(copy, &source, &destination, false);
        }
        SDL_EndGPUCopyPass(copy);
    }

    const SDL_GPUColorTargetInfo colorTarget{
        target.colorTexture,
        0,
        0,
        {frame.clear.r, frame.clear.g, frame.clear.b, frame.clear.a},
        target.loadOp,
        target.storeOp,
        nullptr,
        0,
        0,
        false,
        false,
        0,
        0};
    SDL_GPURenderPass* render =
        SDL_BeginGPURenderPass(target.commandBuffer, &colorTarget, 1, nullptr);
    if (render == nullptr) {
        error = std::string("semantic 2D render pass creation failed: ") + SDL_GetError();
        return false;
    }
    if (vertexBytes != 0) {
        const SDL_GPUBufferBinding binding{impl_->vertices.buffer, 0};
        SDL_BindGPUVertexBuffers(render, 0, &binding, 1);
    }

    std::size_t firstVertex = 0;
    std::size_t drawIndex = 0;
    for (const SemanticDraw& semanticDraw : frame.draws) {
        const auto& bindings = drawBindings[drawIndex++];
        const Canvas& drawCanvas = canvas(semanticDraw);
        PixelRect semanticScissor{};
        if (!resolve_scissor(drawCanvas, clip(semanticDraw), frame.targetWidth, frame.targetHeight,
                             semanticScissor)) {
            firstVertex += 6U;
            continue;
        }
        const SDL_Rect scissor{semanticScissor.x, semanticScissor.y,
                               static_cast<int>(semanticScissor.width),
                               static_cast<int>(semanticScissor.height)};
        const SDL_GPUViewport viewport{static_cast<float>(drawCanvas.viewport.x),
                                       static_cast<float>(drawCanvas.viewport.y),
                                       static_cast<float>(drawCanvas.viewport.width),
                                       static_cast<float>(drawCanvas.viewport.height),
                                       0.0f,
                                       1.0f};
        SDL_SetGPUViewport(render, &viewport);
        SDL_SetGPUScissor(render, &scissor);
        if (const auto* picture = std::get_if<PictureDraw>(&semanticDraw)) {
            const PictureCommand& command = picture->picture;
            SDL_BindGPUGraphicsPipeline(render, picturePipeline);
            SDL_BindGPUFragmentSamplers(render, 0, bindings.data(), bindings.size());
            PictureStyleUniform style{};
            assign(style.black, command.material.black);
            assign(style.white, command.material.white);
            std::int32_t alphaMask = 0;
            for (std::size_t index = 0; index < command.material.textureCount; ++index) {
                style.colorMix[index] = command.material.textures[index].colorMix;
                style.alphaMix[index] = command.material.textures[index].alphaMix;
                if (command.material.textures[index].hasAlpha)
                    alphaMask |= 1 << index;
            }
            style.control[0] = command.material.textureCount;
            style.control[1] = alphaMask;
            style.opacity[0] = command.opacity;
            SDL_PushGPUFragmentUniformData(target.commandBuffer, 0, &style, sizeof(style));
        } else {
            SDL_BindGPUGraphicsPipeline(render, solidPipeline);
        }
        const CanvasUniform canvasUniform{{drawCanvas.origin.x, drawCanvas.origin.y},
                                          {drawCanvas.extent.x, drawCanvas.extent.y}};
        SDL_PushGPUVertexUniformData(target.commandBuffer, 0, &canvasUniform,
                                     sizeof(canvasUniform));
        SDL_DrawGPUPrimitives(render, 6, 1, firstVertex, 0);
        firstVertex += 6U;
    }
    SDL_EndGPURenderPass(render);
    impl_->encodeSucceeded = true;
    return true;
}

bool Semantic2dPass::complete_encode(bool submitted, std::string& error) noexcept {
    error.clear();
    if (impl_ == nullptr || !impl_->encodeActive) {
        error = "no semantic 2D encode is awaiting completion";
        return false;
    }
    if (submitted && !impl_->encodeSucceeded) {
        error = "failed semantic 2D encode cannot be committed as submitted";
        return false;
    }

    if (submitted) {
        for (auto& [key, image] : impl_->pendingImages) {
            (void)key;
            SDL_ReleaseGPUTransferBuffer(impl_->device, image.upload);
            image.upload = nullptr;
        }
        impl_->images.merge(impl_->pendingImages);
        for (auto it = impl_->images.begin(); it != impl_->images.end();) {
            if (std::find(impl_->currentImages.begin(), impl_->currentImages.end(), it->first) !=
                impl_->currentImages.end()) {
                ++it;
                continue;
            }
            release_image(impl_->device, it->second);
            it = impl_->images.erase(it);
        }
    } else {
        for (const auto& [key, image] : impl_->pendingImages) {
            (void)key;
            release_image(impl_->device, image);
        }
        impl_->pendingImages.clear();
    }
    impl_->currentImages.clear();
    impl_->encodeActive = false;
    impl_->encodeSucceeded = false;
    return true;
}

std::size_t Semantic2dPass::resident_image_count() const noexcept {
    return impl_ != nullptr ? impl_->images.size() : 0;
}

bool Semantic2dPass::render_and_readback(const SemanticFrame& frame, SemanticFramePixels& output,
                                         std::string& error) {
    error.clear();
    output = {};
    if (!initialize(error))
        return false;
    std::size_t readbackBytes = 0;
    if (!multiplication_fits(frame.targetWidth, frame.targetHeight, readbackBytes)) {
        error = "semantic 2D target exceeds SDL GPU limits";
        return false;
    }

    SDL_GPUTextureCreateInfo targetInfo{};
    targetInfo.type = SDL_GPU_TEXTURETYPE_2D;
    targetInfo.format = kImageFormat;
    targetInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    targetInfo.width = frame.targetWidth;
    targetInfo.height = frame.targetHeight;
    targetInfo.layer_count_or_depth = 1;
    targetInfo.num_levels = 1;
    targetInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* target = SDL_CreateGPUTexture(impl_->device, &targetInfo);
    const SDL_GPUTransferBufferCreateInfo downloadInfo{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                                                       static_cast<Uint32>(readbackBytes), 0};
    SDL_GPUTransferBuffer* download = SDL_CreateGPUTransferBuffer(impl_->device, &downloadInfo);
    if (target == nullptr || download == nullptr) {
        if (download != nullptr)
            SDL_ReleaseGPUTransferBuffer(impl_->device, download);
        if (target != nullptr)
            SDL_ReleaseGPUTexture(impl_->device, target);
        error = std::string("semantic 2D readback resource allocation failed: ") + SDL_GetError();
        return false;
    }

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(impl_->device);
    if (commandBuffer == nullptr) {
        SDL_ReleaseGPUTransferBuffer(impl_->device, download);
        SDL_ReleaseGPUTexture(impl_->device, target);
        error = std::string("semantic 2D command acquisition failed: ") + SDL_GetError();
        return false;
    }
    const Semantic2dPassTarget passTarget{commandBuffer, target, kImageFormat, SDL_GPU_LOADOP_CLEAR,
                                          SDL_GPU_STOREOP_STORE};
    if (!encode(frame, passTarget, error)) {
        SDL_CancelGPUCommandBuffer(commandBuffer);
        std::string completionError;
        (void)complete_encode(false, completionError);
        SDL_ReleaseGPUTransferBuffer(impl_->device, download);
        SDL_ReleaseGPUTexture(impl_->device, target);
        return false;
    }

    SDL_GPUCopyPass* downloadPass = SDL_BeginGPUCopyPass(commandBuffer);
    if (downloadPass == nullptr) {
        error = std::string("semantic 2D readback pass creation failed: ") + SDL_GetError();
        SDL_CancelGPUCommandBuffer(commandBuffer);
        std::string completionError;
        (void)complete_encode(false, completionError);
        SDL_ReleaseGPUTransferBuffer(impl_->device, download);
        SDL_ReleaseGPUTexture(impl_->device, target);
        return false;
    }
    const SDL_GPUTextureRegion source{target, 0, 0, 0, 0, 0, frame.targetWidth, frame.targetHeight,
                                      1};
    const SDL_GPUTextureTransferInfo destination{download, 0, frame.targetWidth,
                                                 frame.targetHeight};
    SDL_DownloadFromGPUTexture(downloadPass, &source, &destination);
    SDL_EndGPUCopyPass(downloadPass);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commandBuffer);
    if (fence == nullptr) {
        error = std::string("picture submit failed: ") + SDL_GetError();
        std::string completionError;
        (void)complete_encode(false, completionError);
        SDL_ReleaseGPUTransferBuffer(impl_->device, download);
        SDL_ReleaseGPUTexture(impl_->device, target);
        return false;
    }
    std::string completionError;
    if (!complete_encode(true, completionError)) {
        error = std::string("picture cache commit failed after submit: ") + completionError;
        SDL_ReleaseGPUFence(impl_->device, fence);
        SDL_ReleaseGPUTransferBuffer(impl_->device, download);
        SDL_ReleaseGPUTexture(impl_->device, target);
        return false;
    }
    const bool completed = wait_for_fence(impl_->device, fence);
    SDL_ReleaseGPUFence(impl_->device, fence);
    if (!completed) {
        // A non-progressing device owns reclamation. Returning would run this pass's destructor
        // and issue more SDL GPU calls against the same stuck device, so fail at the observed
        // boundary and let the guarded launcher terminate the exact process.
        std::terminate();
    }

    void* mapped = SDL_MapGPUTransferBuffer(impl_->device, download, false);
    if (mapped == nullptr) {
        error = std::string("semantic 2D readback map failed: ") + SDL_GetError();
        SDL_ReleaseGPUTransferBuffer(impl_->device, download);
        SDL_ReleaseGPUTexture(impl_->device, target);
        return false;
    }
    output.width = frame.targetWidth;
    output.height = frame.targetHeight;
    output.rgba8.resize(readbackBytes);
    std::memcpy(output.rgba8.data(), mapped, readbackBytes);
    SDL_UnmapGPUTransferBuffer(impl_->device, download);
    SDL_ReleaseGPUTransferBuffer(impl_->device, download);
    SDL_ReleaseGPUTexture(impl_->device, target);
    return true;
}

} // namespace sb::native_render
