#include <sunbright/native_render/picture_pass.h>

#include "../shaders/picture_frag_spv.h"
#include "../shaders/picture_vert_spv.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>
#include <unordered_map>
#include <utility>

namespace sb::native_render {
namespace {

constexpr SDL_GPUTextureFormat kColorFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
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

struct GpuImage {
    DecodedImageView source{};
    std::size_t uploadOffset = 0;
    SDL_GPUTexture* texture = nullptr;
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

bool wait_for_fence(SDL_GPUDevice* device, SDL_GPUFence* fence) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + kFenceTimeout;
    while (!SDL_QueryGPUFence(device, fence)) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

std::size_t aligned(std::size_t value, std::size_t alignment) noexcept {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

bool multiplication_fits(std::uint32_t width, std::uint32_t height, std::size_t& bytes) noexcept {
    const std::uint64_t total = static_cast<std::uint64_t>(width) * height * 4U;
    if (width == 0 || height == 0 || total > std::numeric_limits<Uint32>::max())
        return false;
    bytes = static_cast<std::size_t>(total);
    return true;
}

} // namespace

struct PicturePass::Impl {
    explicit Impl(SDL_GPUDevice* value) noexcept : device(value) {}

    SDL_GPUDevice* device = nullptr;
    SDL_GPUShader* vertexShader = nullptr;
    SDL_GPUShader* fragmentShader = nullptr;
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
};

PicturePass::PicturePass(SDL_GPUDevice* device) : impl_(new Impl(device)) {}

PicturePass::~PicturePass() {
    if (impl_ != nullptr && impl_->device != nullptr) {
        if (impl_->pipeline != nullptr)
            SDL_ReleaseGPUGraphicsPipeline(impl_->device, impl_->pipeline);
        if (impl_->fragmentShader != nullptr)
            SDL_ReleaseGPUShader(impl_->device, impl_->fragmentShader);
        if (impl_->vertexShader != nullptr)
            SDL_ReleaseGPUShader(impl_->device, impl_->vertexShader);
    }
    delete impl_;
}

bool PicturePass::initialize(std::string& error) {
    error.clear();
    if (impl_ == nullptr || impl_->device == nullptr) {
        error = "picture pass requires a host-owned SDL GPU device";
        return false;
    }
    if (impl_->pipeline != nullptr)
        return true;

    impl_->vertexShader = make_shader(impl_->device, kPictureVertSpv, sizeof(kPictureVertSpv),
                                      SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    impl_->fragmentShader = make_shader(impl_->device, kPictureFragSpv, sizeof(kPictureFragSpv),
                                        SDL_GPU_SHADERSTAGE_FRAGMENT, 4, 1);
    if (impl_->vertexShader == nullptr || impl_->fragmentShader == nullptr) {
        error = std::string("picture shader creation failed: ") + SDL_GetError();
        if (impl_->fragmentShader != nullptr)
            SDL_ReleaseGPUShader(impl_->device, impl_->fragmentShader);
        if (impl_->vertexShader != nullptr)
            SDL_ReleaseGPUShader(impl_->device, impl_->vertexShader);
        impl_->fragmentShader = nullptr;
        impl_->vertexShader = nullptr;
        return false;
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
    colorTarget.format = kColorFormat;
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
    info.vertex_shader = impl_->vertexShader;
    info.fragment_shader = impl_->fragmentShader;
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
    impl_->pipeline = SDL_CreateGPUGraphicsPipeline(impl_->device, &info);
    if (impl_->pipeline == nullptr) {
        error = std::string("picture pipeline creation failed: ") + SDL_GetError();
        SDL_ReleaseGPUShader(impl_->device, impl_->fragmentShader);
        SDL_ReleaseGPUShader(impl_->device, impl_->vertexShader);
        impl_->fragmentShader = nullptr;
        impl_->vertexShader = nullptr;
        return false;
    }
    return true;
}

bool PicturePass::render_and_readback(const PictureFrame& frame, PictureFramePixels& output,
                                      std::string& error) {
    error.clear();
    output = {};
    if (!initialize(error))
        return false;
    PixelRect fullScissor{};
    if (!resolve_scissor(frame.canvas, {}, fullScissor)) {
        error = "picture frame has an invalid canvas";
        return false;
    }
    for (const PictureCommand& command : frame.commands) {
        if (!valid(command)) {
            error = "picture frame contains an invalid command";
            return false;
        }
    }

    std::unordered_map<ImageKey, const DecodedImageView*, ImageKeyHash> sourceImages;
    for (const DecodedImageView& image : frame.images) {
        std::size_t bytes = 0;
        if (image.resource == 0 || !multiplication_fits(image.width, image.height, bytes) ||
            image.rgba8.size() != bytes ||
            !sourceImages.emplace(ImageKey{image.resource, image.revision}, &image).second) {
            error = "picture frame contains an invalid or duplicate image";
            return false;
        }
    }
    for (const PictureCommand& command : frame.commands) {
        for (std::size_t index = 0; index < command.material.textureCount; ++index) {
            const PictureTexture& texture = command.material.textures[index];
            const auto found = sourceImages.find({texture.resource, texture.revision});
            if (found == sourceImages.end() || found->second->width != texture.width ||
                found->second->height != texture.height) {
                error = "picture command references an absent or mismatched decoded image";
                return false;
            }
        }
    }

    std::vector<GpuVertex> vertices;
    vertices.reserve(frame.commands.size() * 6);
    for (const PictureCommand& command : frame.commands) {
        for (const PictureVertex& source : make_mesh(command)) {
            vertices.push_back({{source.position.x, source.position.y},
                                {source.uv.x, source.uv.y},
                                {source.color.r, source.color.g, source.color.b, source.color.a}});
        }
    }
    const std::size_t vertexBytes = vertices.size() * sizeof(GpuVertex);
    if (vertexBytes > std::numeric_limits<Uint32>::max()) {
        error = "picture vertex upload exceeds SDL GPU limits";
        return false;
    }

    std::size_t imageUploadBytes = 0;
    std::unordered_map<ImageKey, std::size_t, ImageKeyHash> imageOffsets;
    for (const DecodedImageView& image : frame.images) {
        imageUploadBytes = aligned(imageUploadBytes, 512);
        imageOffsets.emplace(ImageKey{image.resource, image.revision}, imageUploadBytes);
        imageUploadBytes += image.rgba8.size();
    }
    if (imageUploadBytes > std::numeric_limits<Uint32>::max()) {
        error = "picture image upload exceeds SDL GPU limits";
        return false;
    }
    std::size_t readbackBytes = 0;
    if (!multiplication_fits(frame.canvas.targetWidth, frame.canvas.targetHeight, readbackBytes)) {
        error = "picture target exceeds SDL GPU limits";
        return false;
    }

    SDL_GPUTextureCreateInfo targetInfo{};
    targetInfo.type = SDL_GPU_TEXTURETYPE_2D;
    targetInfo.format = kColorFormat;
    targetInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    targetInfo.width = frame.canvas.targetWidth;
    targetInfo.height = frame.canvas.targetHeight;
    targetInfo.layer_count_or_depth = 1;
    targetInfo.num_levels = 1;
    targetInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* target = SDL_CreateGPUTexture(impl_->device, &targetInfo);

    SDL_GPUBuffer* vertexBuffer = nullptr;
    SDL_GPUTransferBuffer* vertexUpload = nullptr;
    if (vertexBytes != 0) {
        const SDL_GPUBufferCreateInfo bufferInfo{SDL_GPU_BUFFERUSAGE_VERTEX,
                                                 static_cast<Uint32>(vertexBytes), 0};
        vertexBuffer = SDL_CreateGPUBuffer(impl_->device, &bufferInfo);
        const SDL_GPUTransferBufferCreateInfo uploadInfo{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                                         static_cast<Uint32>(vertexBytes), 0};
        vertexUpload = SDL_CreateGPUTransferBuffer(impl_->device, &uploadInfo);
    }
    SDL_GPUTransferBuffer* imageUpload = nullptr;
    if (imageUploadBytes != 0) {
        const SDL_GPUTransferBufferCreateInfo uploadInfo{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                                         static_cast<Uint32>(imageUploadBytes), 0};
        imageUpload = SDL_CreateGPUTransferBuffer(impl_->device, &uploadInfo);
    }
    const SDL_GPUTransferBufferCreateInfo downloadInfo{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                                                       static_cast<Uint32>(readbackBytes), 0};
    SDL_GPUTransferBuffer* download = SDL_CreateGPUTransferBuffer(impl_->device, &downloadInfo);

    std::vector<GpuImage> gpuImages;
    std::vector<SDL_GPUSampler*> samplers;
    std::vector<std::array<SDL_GPUTextureSamplerBinding, 4>> drawBindings;
    gpuImages.reserve(frame.images.size());
    samplers.reserve(frame.commands.size() * 4);
    drawBindings.reserve(frame.commands.size());
    bool resourcesValid =
        target != nullptr && download != nullptr &&
        (vertexBytes == 0 || (vertexBuffer != nullptr && vertexUpload != nullptr)) &&
        (imageUploadBytes == 0 || imageUpload != nullptr);
    for (const DecodedImageView& image : frame.images) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = kColorFormat;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = image.width;
        info.height = image.height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        const auto offset = imageOffsets.find({image.resource, image.revision});
        GpuImage gpuImage{image, offset->second, SDL_CreateGPUTexture(impl_->device, &info)};
        resourcesValid = resourcesValid && gpuImage.texture != nullptr;
        gpuImages.push_back(gpuImage);
    }
    const auto release_resources = [&] {
        for (SDL_GPUSampler* sampler : samplers) {
            if (sampler != nullptr)
                SDL_ReleaseGPUSampler(impl_->device, sampler);
        }
        for (const GpuImage& image : gpuImages) {
            if (image.texture != nullptr)
                SDL_ReleaseGPUTexture(impl_->device, image.texture);
        }
        if (download != nullptr)
            SDL_ReleaseGPUTransferBuffer(impl_->device, download);
        if (imageUpload != nullptr)
            SDL_ReleaseGPUTransferBuffer(impl_->device, imageUpload);
        if (vertexUpload != nullptr)
            SDL_ReleaseGPUTransferBuffer(impl_->device, vertexUpload);
        if (vertexBuffer != nullptr)
            SDL_ReleaseGPUBuffer(impl_->device, vertexBuffer);
        if (target != nullptr)
            SDL_ReleaseGPUTexture(impl_->device, target);
    };
    if (!resourcesValid) {
        error = std::string("picture resource allocation failed: ") + SDL_GetError();
        release_resources();
        return false;
    }
    for (const PictureCommand& command : frame.commands) {
        std::array<SDL_GPUTextureSamplerBinding, 4> bindings{};
        for (std::size_t index = 0; index < bindings.size(); ++index) {
            const PictureTexture& texture =
                command.material
                    .textures[std::min<std::size_t>(index, command.material.textureCount - 1U)];
            const auto gpu =
                std::find_if(gpuImages.begin(), gpuImages.end(), [&](const GpuImage& image) {
                    return image.source.resource == texture.resource &&
                           image.source.revision == texture.revision;
                });
            if (gpu == gpuImages.end()) {
                error = "picture image disappeared during binding";
                release_resources();
                return false;
            }
            SDL_GPUSamplerCreateInfo info{};
            info.min_filter = filter(texture.minFilter);
            info.mag_filter = filter(texture.magFilter);
            info.mipmap_mode = mip_filter(texture.mipFilter);
            info.address_mode_u = address_mode(texture.addressU);
            info.address_mode_v = address_mode(texture.addressV);
            info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            info.max_lod = 0.0f;
            SDL_GPUSampler* sampler = SDL_CreateGPUSampler(impl_->device, &info);
            if (sampler == nullptr) {
                error = std::string("picture sampler creation failed: ") + SDL_GetError();
                release_resources();
                return false;
            }
            samplers.push_back(sampler);
            bindings[index] = {gpu->texture, sampler};
        }
        drawBindings.push_back(bindings);
    }

    if (vertexBytes != 0) {
        void* mapped = SDL_MapGPUTransferBuffer(impl_->device, vertexUpload, false);
        if (mapped == nullptr) {
            error = std::string("picture vertex upload map failed: ") + SDL_GetError();
            release_resources();
            return false;
        }
        std::memcpy(mapped, vertices.data(), vertexBytes);
        SDL_UnmapGPUTransferBuffer(impl_->device, vertexUpload);
    }
    if (imageUploadBytes != 0) {
        auto* mapped =
            static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(impl_->device, imageUpload, false));
        if (mapped == nullptr) {
            error = std::string("picture image upload map failed: ") + SDL_GetError();
            release_resources();
            return false;
        }
        for (const GpuImage& image : gpuImages) {
            std::memcpy(mapped + image.uploadOffset, image.source.rgba8.data(),
                        image.source.rgba8.size());
        }
        SDL_UnmapGPUTransferBuffer(impl_->device, imageUpload);
    }

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(impl_->device);
    if (commandBuffer == nullptr) {
        error = std::string("picture command acquisition failed: ") + SDL_GetError();
        release_resources();
        return false;
    }
    if (vertexBytes != 0 || imageUploadBytes != 0) {
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commandBuffer);
        if (vertexBytes != 0) {
            const SDL_GPUTransferBufferLocation source{vertexUpload, 0};
            const SDL_GPUBufferRegion destination{vertexBuffer, 0,
                                                  static_cast<Uint32>(vertexBytes)};
            SDL_UploadToGPUBuffer(copy, &source, &destination, false);
        }
        for (const GpuImage& image : gpuImages) {
            const SDL_GPUTextureTransferInfo source{
                imageUpload,
                static_cast<Uint32>(image.uploadOffset),
                image.source.width,
                image.source.height,
            };
            const SDL_GPUTextureRegion destination{
                image.texture, 0, 0, 0, 0, 0, image.source.width, image.source.height, 1};
            SDL_UploadToGPUTexture(copy, &source, &destination, false);
        }
        SDL_EndGPUCopyPass(copy);
    }

    const SDL_GPUColorTargetInfo colorTarget{
        target,
        0,
        0,
        {frame.clear.r, frame.clear.g, frame.clear.b, frame.clear.a},
        SDL_GPU_LOADOP_CLEAR,
        SDL_GPU_STOREOP_STORE,
        nullptr,
        0,
        0,
        false,
        false,
        0,
        0};
    SDL_GPURenderPass* render = SDL_BeginGPURenderPass(commandBuffer, &colorTarget, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(render, impl_->pipeline);
    if (vertexBytes != 0) {
        const SDL_GPUBufferBinding binding{vertexBuffer, 0};
        SDL_BindGPUVertexBuffers(render, 0, &binding, 1);
    }
    const CanvasUniform canvas{{frame.canvas.origin.x, frame.canvas.origin.y},
                               {frame.canvas.extent.x, frame.canvas.extent.y}};
    std::size_t firstVertex = 0;
    std::size_t drawIndex = 0;
    for (const PictureCommand& command : frame.commands) {
        const auto& bindings = drawBindings[drawIndex++];
        PixelRect semanticScissor{};
        if (!resolve_scissor(frame.canvas, command.clip, semanticScissor)) {
            // A valid picture may be wholly outside its active clip. It contributes no pixels but
            // remains part of ordering, so consume its vertices without treating it as malformed.
            firstVertex += 6;
            continue;
        }
        const SDL_Rect scissor{semanticScissor.x, semanticScissor.y,
                               static_cast<int>(semanticScissor.width),
                               static_cast<int>(semanticScissor.height)};
        SDL_SetGPUScissor(render, &scissor);

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
        SDL_PushGPUVertexUniformData(commandBuffer, 0, &canvas, sizeof(canvas));
        SDL_PushGPUFragmentUniformData(commandBuffer, 0, &style, sizeof(style));
        SDL_DrawGPUPrimitives(render, 6, 1, firstVertex, 0);
        firstVertex += 6;
    }
    SDL_EndGPURenderPass(render);

    SDL_GPUCopyPass* downloadPass = SDL_BeginGPUCopyPass(commandBuffer);
    const SDL_GPUTextureRegion source{
        target, 0, 0, 0, 0, 0, frame.canvas.targetWidth, frame.canvas.targetHeight, 1};
    const SDL_GPUTextureTransferInfo destination{download, 0, frame.canvas.targetWidth,
                                                 frame.canvas.targetHeight};
    SDL_DownloadFromGPUTexture(downloadPass, &source, &destination);
    SDL_EndGPUCopyPass(downloadPass);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commandBuffer);
    if (fence == nullptr) {
        error = std::string("picture submit failed: ") + SDL_GetError();
        release_resources();
        return false;
    }
    const bool completed = wait_for_fence(impl_->device, fence);
    SDL_ReleaseGPUFence(impl_->device, fence);
    if (!completed) {
        error = "picture pass GPU fence did not signal within five seconds";
        // A non-progressing device owns resource reclamation; do not issue further calls here.
        return false;
    }

    void* mapped = SDL_MapGPUTransferBuffer(impl_->device, download, false);
    if (mapped == nullptr) {
        error = std::string("picture readback map failed: ") + SDL_GetError();
        release_resources();
        return false;
    }
    output.width = frame.canvas.targetWidth;
    output.height = frame.canvas.targetHeight;
    output.rgba8.resize(readbackBytes);
    std::memcpy(output.rgba8.data(), mapped, readbackBytes);
    SDL_UnmapGPUTransferBuffer(impl_->device, download);
    release_resources();
    return true;
}

} // namespace sb::native_render
