#include "native_gpu_pipeline.h"

#include "shaders/geom_frag_spv.h"
#include "shaders/geom_vert_spv.h"

#include <lucent/log.h>

#include <cstdlib>
#include <unordered_map>

namespace {

SDL_GPUDevice* g_device = nullptr;
SDL_GPUShader* g_vertexShader = nullptr;
SDL_GPUShader* g_fragmentShader = nullptr;
std::unordered_map<uint32_t, SDL_GPUGraphicsPipeline*> g_pipelines;

SDL_GPUShader* make_shader(const void* code, size_t bytes, SDL_GPUShaderStage stage,
                           Uint32 samplers, Uint32 uniformBuffers) {
    SDL_GPUShaderCreateInfo info{};
    info.code = static_cast<const Uint8*>(code);
    info.code_size = bytes;
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = stage;
    info.num_samplers = samplers;
    info.num_uniform_buffers = uniformBuffers;
    return SDL_CreateGPUShader(g_device, &info);
}

SDL_GPUCompareOp gx_compare(uint8_t function) {
    switch (function & 7) {
    case 0:
        return SDL_GPU_COMPAREOP_NEVER;
    case 1:
        return SDL_GPU_COMPAREOP_LESS;
    case 2:
        return SDL_GPU_COMPAREOP_EQUAL;
    case 3:
        return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    case 4:
        return SDL_GPU_COMPAREOP_GREATER;
    case 5:
        return SDL_GPU_COMPAREOP_NOT_EQUAL;
    case 6:
        return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    default:
        return SDL_GPU_COMPAREOP_ALWAYS;
    }
}

uint32_t make_pipeline_key(SbrDepthState state) {
    return static_cast<uint32_t>(state.test != 0) << 24 |
           static_cast<uint32_t>(state.func & 7) << 20 |
           static_cast<uint32_t>(state.write != 0) << 16 |
           static_cast<uint32_t>(state.blend & 3) << 8 |
           static_cast<uint32_t>(state.srcFac & 7) << 4 | static_cast<uint32_t>(state.dstFac & 7) |
           static_cast<uint32_t>(state.colorUpdate != 0) << 28 |
           static_cast<uint32_t>(state.alphaUpdate != 0) << 29 |
           static_cast<uint32_t>(state.cull & 3) << 30;
}

SDL_GPUBlendFactor gx_blend_factor(uint8_t factor) {
    switch (factor & 7) {
    case 0:
        return SDL_GPU_BLENDFACTOR_ZERO;
    case 1:
        return SDL_GPU_BLENDFACTOR_ONE;
    case 2:
        return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case 3:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case 4:
        return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case 5:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case 6:
        return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    default:
        return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    }
}

} // namespace

bool sbr_native_gpu_pipeline_init(SDL_GPUDevice* device) {
    sbr_native_gpu_pipeline_shutdown();
    if (device == nullptr)
        return false;
    g_device = device;
    g_vertexShader =
        make_shader(kGeomVertSpv, sizeof(kGeomVertSpv), SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    g_fragmentShader =
        make_shader(kGeomFragSpv, sizeof(kGeomFragSpv), SDL_GPU_SHADERSTAGE_FRAGMENT, 8, 1);
    g_pipelines.clear();
    if (g_vertexShader == nullptr || g_fragmentShader == nullptr) {
        sbr_native_gpu_pipeline_shutdown();
        return false;
    }
    return true;
}

void sbr_native_gpu_pipeline_shutdown() noexcept {
    if (g_device != nullptr) {
        for (auto& [key, pipeline] : g_pipelines) {
            (void)key;
            if (pipeline != nullptr)
                SDL_ReleaseGPUGraphicsPipeline(g_device, pipeline);
        }
        if (g_fragmentShader != nullptr)
            SDL_ReleaseGPUShader(g_device, g_fragmentShader);
        if (g_vertexShader != nullptr)
            SDL_ReleaseGPUShader(g_device, g_vertexShader);
    }
    g_pipelines.clear();
    g_fragmentShader = nullptr;
    g_vertexShader = nullptr;
    g_device = nullptr;
}

uint32_t sbr_native_gpu_pipeline_key(SbrDepthState state) {
    return make_pipeline_key(state);
}

SDL_GPUGraphicsPipeline* sbr_native_gpu_pipeline_for(SbrDepthState state) {
    const uint32_t key = make_pipeline_key(state);
    if (const auto found = g_pipelines.find(key); found != g_pipelines.end())
        return found->second;

    SDL_GPUVertexBufferDescription vertexBuffer{};
    vertexBuffer.slot = 0;
    vertexBuffer.pitch = sizeof(SbrVertex);
    vertexBuffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute attributes[5]{};
    for (unsigned index = 0; index < 5; ++index) {
        attributes[index].location = index;
        attributes[index].buffer_slot = 0;
        attributes[index].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attributes[index].offset = index * 16;
    }

    SDL_GPUColorTargetDescription colorTarget{};
    colorTarget.format = kNativeColorFormat;
    colorTarget.blend_state.enable_color_write_mask = true;
    colorTarget.blend_state.color_write_mask = static_cast<SDL_GPUColorComponentFlags>(
        (state.colorUpdate
             ? SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B
             : 0) |
        (state.alphaUpdate ? SDL_GPU_COLORCOMPONENT_A : 0));
    if (state.blend == 1) {
        colorTarget.blend_state.enable_blend = true;
        colorTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        colorTarget.blend_state.src_color_blendfactor = gx_blend_factor(state.srcFac);
        colorTarget.blend_state.dst_color_blendfactor = gx_blend_factor(state.dstFac);
        colorTarget.blend_state.src_alpha_blendfactor = gx_blend_factor(state.srcFac);
        colorTarget.blend_state.dst_alpha_blendfactor = gx_blend_factor(state.dstFac);
    }

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = g_vertexShader;
    info.fragment_shader = g_fragmentShader;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.vertex_input_state.vertex_buffer_descriptions = &vertexBuffer;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attributes;
    info.vertex_input_state.num_vertex_attributes = 5;
    const char* wireframe = std::getenv("SBR_RENDER_WIREFRAME");
    info.rasterizer_state.fill_mode =
        wireframe != nullptr && wireframe[0] != '\0' && wireframe[0] != '0' ? SDL_GPU_FILLMODE_LINE
                                                                            : SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
    info.rasterizer_state.cull_mode = state.cull == 1   ? SDL_GPU_CULLMODE_FRONT
                                      : state.cull == 2 ? SDL_GPU_CULLMODE_BACK
                                      : state.cull == 3 ? SDL_GPU_CULLMODE_BACK
                                                        : SDL_GPU_CULLMODE_NONE;
    info.target_info.color_target_descriptions = &colorTarget;
    info.target_info.num_color_targets = 1;
    info.target_info.depth_stencil_format = kNativeDepthFormat;
    info.target_info.has_depth_stencil_target = true;
    info.depth_stencil_state.enable_depth_test = state.test != 0;
    info.depth_stencil_state.enable_depth_write = state.write != 0;
    info.depth_stencil_state.compare_op = gx_compare(state.func);

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(g_device, &info);
    if (pipeline == nullptr) {
        lucent::error(
            "nrender",
            "pipeline create failed for test={} func={} write={} blend={} src={} dst={}: {}",
            state.test, state.func, state.write, state.blend, state.srcFac, state.dstFac,
            SDL_GetError());
        std::abort();
    }
    g_pipelines.emplace(key, pipeline);
    return pipeline;
}
