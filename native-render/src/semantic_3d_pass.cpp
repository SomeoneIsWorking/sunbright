#include <sunbright/native_render/semantic_3d_pass.h>

#include "../shaders/model_color_frag_spv.h"
#include "../shaders/model_layered_lit_frag_spv.h"
#include "../shaders/model_lit_alpha_mask_frag_spv.h"
#include "../shaders/model_texture_frag_spv.h"
#include "../shaders/model_vert_spv.h"
#include "sdl_image_cache.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <unordered_map>
#include <vector>

namespace sb::native_render {
namespace {

struct GpuVertex {
    float position[4];
    float uv[2];
    float color[4];
    float additiveColor[4];
    float uv1[2];
    float detailTextureWeight;
    float eyeDepth;
};

enum class ModelShaderKind : std::uint8_t { Color, Texture, LitAlphaMask, LayeredLit };

struct DrawBatch {
    Uint32 firstVertex = 0;
    Uint32 vertexCount = 0;
    ModelShaderKind shader = ModelShaderKind::Color;
    ModelRasterPolicy raster{};
    ModelFog fog{};
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    std::array<SDL_GPUTextureSamplerBinding, 2> textures{};
    Uint32 textureCount = 0;
};

struct ModelRasterUniform {
    float alphaTest[4]{};
    float fogRange[4]{};
    float fogColor[4]{};
};

static_assert(sizeof(ModelRasterUniform) == 48);

struct VertexStorage {
    SDL_GPUBuffer* buffer = nullptr;
    SDL_GPUTransferBuffer* upload = nullptr;
    std::size_t capacity = 0;
};

struct PipelineKey {
    SDL_GPUTextureFormat color = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTextureFormat depth = SDL_GPU_TEXTUREFORMAT_INVALID;
    ModelShaderKind shader = ModelShaderKind::Color;
    ModelRasterPolicy raster{};
    bool operator==(const PipelineKey&) const = default;
};

struct PipelineKeyHash {
    std::size_t operator()(PipelineKey key) const noexcept {
        std::size_t value =
            static_cast<std::size_t>(key.color) | (static_cast<std::size_t>(key.depth) << 16U);
        const auto append = [&](std::size_t field) { value = (value * 131U) ^ field; };
        append(static_cast<std::size_t>(key.shader));
        append(static_cast<std::size_t>(key.raster.cull));
        append(key.raster.depthTest);
        append(static_cast<std::size_t>(key.raster.depthCompare));
        append(key.raster.depthWrite);
        append(static_cast<std::size_t>(key.raster.alphaTest));
        append(static_cast<std::size_t>(key.raster.blend));
        return value;
    }
};

struct MeshKey {
    std::uint64_t resource = 0;
    std::uint64_t revision = 0;
    bool operator==(const MeshKey&) const = default;
};

struct MeshKeyHash {
    std::size_t operator()(MeshKey key) const noexcept {
        return std::hash<std::uint64_t>{}(key.resource) ^
               (std::hash<std::uint64_t>{}(key.revision) << 1U);
    }
};

std::size_t next_capacity(std::size_t required) noexcept {
    std::size_t capacity = 256;
    while (capacity < required && capacity <= std::numeric_limits<Uint32>::max() / 2U)
        capacity *= 2U;
    return std::max(capacity, required);
}

void release_storage(SDL_GPUDevice* device, VertexStorage storage) noexcept {
    if (storage.upload != nullptr)
        SDL_ReleaseGPUTransferBuffer(device, storage.upload);
    if (storage.buffer != nullptr)
        SDL_ReleaseGPUBuffer(device, storage.buffer);
}

SDL_GPUShader* make_shader(SDL_GPUDevice* device, const void* code, std::size_t bytes,
                           SDL_GPUShaderStage stage, Uint32 samplers = 0,
                           Uint32 uniformBuffers = 0) noexcept {
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

SDL_GPUCullMode cull_mode(ModelCullMode mode) noexcept {
    switch (mode) {
    case ModelCullMode::None:
        return SDL_GPU_CULLMODE_NONE;
    case ModelCullMode::Front:
        return SDL_GPU_CULLMODE_FRONT;
    case ModelCullMode::Back:
    case ModelCullMode::All:
        return SDL_GPU_CULLMODE_BACK;
    }
    return SDL_GPU_CULLMODE_NONE;
}

SDL_GPUCompareOp depth_compare(ModelDepthCompare compare) noexcept {
    constexpr std::array operations{
        SDL_GPU_COMPAREOP_NEVER,
        SDL_GPU_COMPAREOP_LESS,
        SDL_GPU_COMPAREOP_EQUAL,
        SDL_GPU_COMPAREOP_LESS_OR_EQUAL,
        SDL_GPU_COMPAREOP_GREATER,
        SDL_GPU_COMPAREOP_NOT_EQUAL,
        SDL_GPU_COMPAREOP_GREATER_OR_EQUAL,
        SDL_GPU_COMPAREOP_ALWAYS,
    };
    return operations[static_cast<std::size_t>(compare)];
}

float alpha_threshold(ModelAlphaTest test) noexcept {
    return test == ModelAlphaTest::GreaterOrEqualHalf ? 128.0F / 255.0F : 0.0F;
}

} // namespace

struct Semantic3dPassImpl {
    explicit Semantic3dPassImpl(SDL_GPUDevice* value) noexcept : device(value), images(value) {}

    SDL_GPUDevice* device = nullptr;
    SDL_GPUShader* vertexShader = nullptr;
    SDL_GPUShader* colorFragmentShader = nullptr;
    SDL_GPUShader* textureFragmentShader = nullptr;
    SDL_GPUShader* litAlphaMaskFragmentShader = nullptr;
    SDL_GPUShader* layeredLitFragmentShader = nullptr;
    std::unordered_map<PipelineKey, SDL_GPUGraphicsPipeline*, PipelineKeyHash> pipelines{};
    VertexStorage vertices{};
    std::vector<VertexStorage> retiredStorage{};
    SdlImageCache images;
    bool encodeActive = false;
    bool encodeSucceeded = false;
};

namespace {

bool ensure_storage(Semantic3dPassImpl& impl, std::size_t required, std::string& error) {
    if (required == 0 || required <= impl.vertices.capacity)
        return true;
    const std::size_t capacity = next_capacity(required);
    if (capacity > std::numeric_limits<Uint32>::max()) {
        error = "semantic 3D vertex upload exceeds SDL GPU limits";
        return false;
    }
    const SDL_GPUBufferCreateInfo bufferInfo{SDL_GPU_BUFFERUSAGE_VERTEX,
                                             static_cast<Uint32>(capacity), 0};
    const SDL_GPUTransferBufferCreateInfo uploadInfo{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                                     static_cast<Uint32>(capacity), 0};
    VertexStorage replacement{SDL_CreateGPUBuffer(impl.device, &bufferInfo),
                              SDL_CreateGPUTransferBuffer(impl.device, &uploadInfo), capacity};
    if (replacement.buffer == nullptr || replacement.upload == nullptr) {
        release_storage(impl.device, replacement);
        error = std::string("semantic 3D vertex resource allocation failed: ") + SDL_GetError();
        return false;
    }
    if (impl.vertices.buffer != nullptr)
        impl.retiredStorage.push_back(impl.vertices);
    impl.vertices = replacement;
    return true;
}

SDL_GPUGraphicsPipeline* ensure_pipeline(Semantic3dPassImpl& impl, PipelineKey key,
                                         std::string& error) {
    if (const auto existing = impl.pipelines.find(key); existing != impl.pipelines.end())
        return existing->second;
    if (key.color == SDL_GPU_TEXTUREFORMAT_INVALID || key.depth == SDL_GPU_TEXTUREFORMAT_INVALID) {
        error = "semantic 3D target formats are invalid";
        return nullptr;
    }

    const SDL_GPUVertexBufferDescription vertexBuffer{0, sizeof(GpuVertex),
                                                      SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
    const std::array attributes{
        SDL_GPUVertexAttribute{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
                               offsetof(GpuVertex, position)},
        SDL_GPUVertexAttribute{1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GpuVertex, uv)},
        SDL_GPUVertexAttribute{2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
                               offsetof(GpuVertex, color)},
        SDL_GPUVertexAttribute{3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,
                               offsetof(GpuVertex, additiveColor)},
        SDL_GPUVertexAttribute{4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(GpuVertex, uv1)},
        SDL_GPUVertexAttribute{5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT,
                               offsetof(GpuVertex, detailTextureWeight)},
        SDL_GPUVertexAttribute{6, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT,
                               offsetof(GpuVertex, eyeDepth)},
    };
    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = key.color;
    colorTarget.blend_state.color_write_mask = static_cast<SDL_GPUColorComponentFlags>(
        SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B |
        SDL_GPU_COLORCOMPONENT_A);
    colorTarget.blend_state.enable_color_write_mask = true;

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = impl.vertexShader;
    switch (key.shader) {
    case ModelShaderKind::Color:
        info.fragment_shader = impl.colorFragmentShader;
        break;
    case ModelShaderKind::Texture:
        info.fragment_shader = impl.textureFragmentShader;
        break;
    case ModelShaderKind::LitAlphaMask:
        info.fragment_shader = impl.litAlphaMaskFragmentShader;
        break;
    case ModelShaderKind::LayeredLit:
        info.fragment_shader = impl.layeredLitFragmentShader;
        break;
    }
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.vertex_input_state.vertex_buffer_descriptions = &vertexBuffer;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attributes.data();
    info.vertex_input_state.num_vertex_attributes = attributes.size();
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = cull_mode(key.raster.cull);
    // J3D/GX-authored front faces are clockwise. The semantic policy preserves that authored
    // winding convention while carrying no GX register encoding into this pass.
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
    info.target_info.color_target_descriptions = &colorTarget;
    info.target_info.num_color_targets = 1;
    info.target_info.depth_stencil_format = key.depth;
    info.target_info.has_depth_stencil_target = true;
    info.depth_stencil_state.enable_depth_test = key.raster.depthTest;
    info.depth_stencil_state.enable_depth_write = key.raster.depthWrite;
    info.depth_stencil_state.compare_op = depth_compare(key.raster.depthCompare);
    if (key.raster.blend != ModelBlendMode::Replace) {
        colorTarget.blend_state.enable_blend = true;
        colorTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        const SDL_GPUBlendFactor source = key.raster.blend == ModelBlendMode::SourceAlpha
                                              ? SDL_GPU_BLENDFACTOR_SRC_ALPHA
                                              : SDL_GPU_BLENDFACTOR_ONE;
        colorTarget.blend_state.src_color_blendfactor = source;
        colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorTarget.blend_state.src_alpha_blendfactor = source;
        colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    }
    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(impl.device, &info);
    if (pipeline == nullptr) {
        error = std::string("semantic 3D pipeline creation failed: ") + SDL_GetError();
        return nullptr;
    }
    impl.pipelines.emplace(key, pipeline);
    return pipeline;
}

} // namespace

Semantic3dPass::Semantic3dPass(SDL_GPUDevice* device) : impl_(new Semantic3dPassImpl(device)) {}

Semantic3dPass::~Semantic3dPass() {
    if (impl_ != nullptr && impl_->encodeActive)
        std::terminate();
    if (impl_ != nullptr && impl_->device != nullptr) {
        for (const auto& [key, pipeline] : impl_->pipelines) {
            (void)key;
            SDL_ReleaseGPUGraphicsPipeline(impl_->device, pipeline);
        }
        release_storage(impl_->device, impl_->vertices);
        for (VertexStorage storage : impl_->retiredStorage)
            release_storage(impl_->device, storage);
        if (impl_->textureFragmentShader != nullptr)
            SDL_ReleaseGPUShader(impl_->device, impl_->textureFragmentShader);
        if (impl_->litAlphaMaskFragmentShader != nullptr)
            SDL_ReleaseGPUShader(impl_->device, impl_->litAlphaMaskFragmentShader);
        if (impl_->layeredLitFragmentShader != nullptr)
            SDL_ReleaseGPUShader(impl_->device, impl_->layeredLitFragmentShader);
        if (impl_->colorFragmentShader != nullptr)
            SDL_ReleaseGPUShader(impl_->device, impl_->colorFragmentShader);
        if (impl_->vertexShader != nullptr)
            SDL_ReleaseGPUShader(impl_->device, impl_->vertexShader);
    }
    delete impl_;
}

bool Semantic3dPass::initialize(std::string& error) {
    error.clear();
    if (impl_ == nullptr || impl_->device == nullptr) {
        error = "semantic 3D pass requires a host-owned SDL GPU device";
        return false;
    }
    if (impl_->vertexShader != nullptr && impl_->colorFragmentShader != nullptr &&
        impl_->textureFragmentShader != nullptr && impl_->litAlphaMaskFragmentShader != nullptr &&
        impl_->layeredLitFragmentShader != nullptr)
        return true;
    impl_->vertexShader = make_shader(impl_->device, kModelVertSpv, sizeof(kModelVertSpv),
                                      SDL_GPU_SHADERSTAGE_VERTEX);
    impl_->colorFragmentShader =
        make_shader(impl_->device, kModelColorFragSpv, sizeof(kModelColorFragSpv),
                    SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    impl_->textureFragmentShader =
        make_shader(impl_->device, kModelTextureFragSpv, sizeof(kModelTextureFragSpv),
                    SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    impl_->litAlphaMaskFragmentShader =
        make_shader(impl_->device, kModelLitAlphaMaskFragSpv, sizeof(kModelLitAlphaMaskFragSpv),
                    SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
    impl_->layeredLitFragmentShader =
        make_shader(impl_->device, kModelLayeredLitFragSpv, sizeof(kModelLayeredLitFragSpv),
                    SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
    if (impl_->vertexShader == nullptr || impl_->colorFragmentShader == nullptr ||
        impl_->textureFragmentShader == nullptr || impl_->litAlphaMaskFragmentShader == nullptr ||
        impl_->layeredLitFragmentShader == nullptr) {
        error = std::string("semantic 3D shader creation failed: ") + SDL_GetError();
        return false;
    }
    return true;
}

bool Semantic3dPass::encode(const SemanticFrame& frame, const Semantic3dPassTarget& target,
                            std::string& error) {
    error.clear();
    if (impl_ == nullptr || impl_->encodeActive) {
        error = "previous semantic 3D encode has not been completed";
        return false;
    }
    impl_->encodeActive = true;
    impl_->encodeSucceeded = false;
    if (!initialize(error))
        return false;
    if (target.commandBuffer == nullptr || target.colorTexture == nullptr ||
        target.depthTexture == nullptr || frame.targetWidth == 0 || frame.targetHeight == 0) {
        error = "semantic 3D encode requires valid command-buffer, targets, and extent";
        return false;
    }
    if (!impl_->images.begin(frame.images, error))
        return false;

    std::unordered_map<MeshKey, const MeshResourceView*, MeshKeyHash> meshes;
    for (const MeshResourceView& mesh : frame.meshes) {
        const MeshKey key{mesh.resource, mesh.revision};
        if (!valid(mesh) || !meshes.emplace(key, &mesh).second) {
            error = "semantic frame contains an invalid or duplicate mesh";
            return false;
        }
    }
    std::vector<GpuVertex> vertices;
    std::vector<DrawBatch> batches;
    for (const ModelDraw& draw : frame.models) {
        const MeshKey key{draw.mesh.resource, draw.mesh.revision};
        const auto mesh = meshes.find(key);
        if (mesh == meshes.end() || !model_mesh_matches(draw, *mesh->second)) {
            error = "semantic model references an absent or mismatched mesh";
            return false;
        }
        const ModelRasterPolicy& raster = raster_policy(draw.material);
        // Cull-all is an authored no-fragment operation. SDL has no cull-both pipeline mode, so
        // omit the batch before upload rather than approximating it with one-sided culling.
        if (raster.cull == ModelCullMode::All)
            continue;
        if (vertices.size() > std::numeric_limits<Uint32>::max() ||
            mesh->second->vertices.size() > std::numeric_limits<Uint32>::max() - vertices.size()) {
            error = "semantic 3D vertex count exceeds SDL GPU limits";
            return false;
        }
        DrawBatch batch{.firstVertex = static_cast<Uint32>(vertices.size()),
                        .vertexCount = static_cast<Uint32>(mesh->second->vertices.size()),
                        .raster = raster,
                        .fog = draw.fog};
        batch.textureCount = material_texture_count(draw.material);
        if (batch.textureCount > batch.textures.size()) {
            error = "semantic model material exceeds the supported image count";
            return false;
        }
        for (Uint32 textureIndex = 0; textureIndex < batch.textureCount; ++textureIndex) {
            const PictureTexture* texture = material_texture(draw.material, textureIndex);
            if (texture == nullptr ||
                !impl_->images.resolve(*texture, batch.textures[textureIndex], error)) {
                return false;
            }
        }
        if (std::holds_alternative<LitTexturedAlphaMaskMaterial>(draw.material))
            batch.shader = ModelShaderKind::LitAlphaMask;
        else if (std::holds_alternative<LitLayeredTexturedMaterial>(draw.material))
            batch.shader = ModelShaderKind::LayeredLit;
        else if (batch.textureCount == 1)
            batch.shader = ModelShaderKind::Texture;
        batch.pipeline = ensure_pipeline(
            *impl_, {target.colorFormat, target.depthFormat, batch.shader, raster}, error);
        if (batch.pipeline == nullptr)
            return false;
        for (const MeshVertex& source : mesh->second->vertices) {
            const ClipVertex transformed = transform_vertex(draw, source);
            vertices.push_back({{transformed.position.x, transformed.position.y,
                                 transformed.position.z, transformed.position.w},
                                {transformed.uv.x, transformed.uv.y},
                                {transformed.color.r, transformed.color.g, transformed.color.b,
                                 transformed.color.a},
                                {transformed.additiveColor.r, transformed.additiveColor.g,
                                 transformed.additiveColor.b, transformed.additiveColor.a},
                                {transformed.uv1.x, transformed.uv1.y},
                                transformed.detailTextureWeight,
                                transformed.eyeDepth});
        }
        batches.push_back(batch);
    }
    const std::size_t bytes = vertices.size() * sizeof(GpuVertex);
    if (!ensure_storage(*impl_, bytes, error))
        return false;
    if (bytes != 0) {
        void* mapped = SDL_MapGPUTransferBuffer(impl_->device, impl_->vertices.upload, true);
        if (mapped == nullptr) {
            error = std::string("semantic 3D vertex upload map failed: ") + SDL_GetError();
            return false;
        }
        std::memcpy(mapped, vertices.data(), bytes);
        SDL_UnmapGPUTransferBuffer(impl_->device, impl_->vertices.upload);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(target.commandBuffer);
        if (copy == nullptr) {
            error = std::string("semantic 3D upload pass creation failed: ") + SDL_GetError();
            return false;
        }
        const SDL_GPUTransferBufferLocation source{impl_->vertices.upload, 0};
        const SDL_GPUBufferRegion destination{impl_->vertices.buffer, 0,
                                              static_cast<Uint32>(bytes)};
        SDL_UploadToGPUBuffer(copy, &source, &destination, true);
        SDL_EndGPUCopyPass(copy);
    }
    if (!impl_->images.encode_uploads(target.commandBuffer, error))
        return false;

    SDL_GPUColorTargetInfo color{};
    color.texture = target.colorTexture;
    color.clear_color = {frame.clear.r, frame.clear.g, frame.clear.b, frame.clear.a};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = target.depthTexture;
    depth.clear_depth = 1.0F;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* render = SDL_BeginGPURenderPass(target.commandBuffer, &color, 1, &depth);
    if (render == nullptr) {
        error = std::string("semantic 3D render pass creation failed: ") + SDL_GetError();
        return false;
    }
    if (bytes != 0) {
        const SDL_GPUBufferBinding binding{impl_->vertices.buffer, 0};
        SDL_BindGPUVertexBuffers(render, 0, &binding, 1);
        for (const DrawBatch& batch : batches) {
            SDL_BindGPUGraphicsPipeline(render, batch.pipeline);
            if (batch.textureCount != 0)
                SDL_BindGPUFragmentSamplers(render, 0, batch.textures.data(), batch.textureCount);
            const bool fogEnabled = batch.fog.mode == ModelFogMode::Linear;
            const float inverseFogRange =
                fogEnabled ? 1.0F / (batch.fog.end - batch.fog.start) : 0.0F;
            const ModelRasterUniform rasterUniform{
                .alphaTest = {alpha_threshold(batch.raster.alphaTest), 0, 0, 0},
                .fogRange = {batch.fog.start, inverseFogRange, fogEnabled ? 1.0F : 0.0F, 0},
                .fogColor = {batch.fog.color.r, batch.fog.color.g, batch.fog.color.b,
                             batch.fog.color.a},
            };
            SDL_PushGPUFragmentUniformData(target.commandBuffer, 0, &rasterUniform,
                                           sizeof(rasterUniform));
            SDL_DrawGPUPrimitives(render, batch.vertexCount, 1, batch.firstVertex, 0);
        }
    }
    SDL_EndGPURenderPass(render);
    impl_->encodeSucceeded = true;
    return true;
}

bool Semantic3dPass::complete_encode(bool submitted, std::string& error) noexcept {
    error.clear();
    if (impl_ == nullptr || !impl_->encodeActive) {
        error = "no semantic 3D encode is awaiting completion";
        return false;
    }
    if (submitted && !impl_->encodeSucceeded) {
        error = "failed semantic 3D encode cannot be committed as submitted";
        return false;
    }
    if (!impl_->images.complete(submitted, error))
        return false;
    impl_->encodeActive = false;
    impl_->encodeSucceeded = false;
    return true;
}

} // namespace sb::native_render
