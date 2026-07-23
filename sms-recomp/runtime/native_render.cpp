// native_render.cpp — the recomp's own GX renderer on SDL3 GPU (2026-07-23, user-directed).
//
// The end state: no Aurora, no Dolphin — the parsed GX stream (dev_gxfifo) rendered by code we own,
// on the SDL3 GPU API. Built INCREMENTALLY alongside aurora, which stays as the per-frame parity
// oracle until this reaches parity (CLAUDE.md RENDERER DOCTRINE, 2026-07-23).
//
// The SDL3-GPU device and the clear/target/readback plumbing are resurrected from the retired
// Path-B renderer (git 9283f44^:native/render/gx_sdlgpu.cpp), which reached real per-material TEV.
// Its BACKEND (batches -> GPU) resurrects here; its FRONTEND (GX state -> transformed verts + TEV
// shaders) has to be driven from dev_gxfifo's FIFO parse, which is the work ahead.
//
// MILESTONE LADDER (each A/B'd against aurora): [0] device + clear + readback  ->  pass-through geom
// ->  vertex transform  ->  TEV  ->  textures  ->  EFB. This file is at milestone 0.
//
//   SBR_SDLGPU=1        stand the device up and, once per frame, clear the native target to the GX
//                       copy-clear colour and read it back (proves the plumbing; nothing drawn yet).

#include "native_render.h"

#include <lucent/log.h>

#include <SDL3/SDL.h>

#include "shaders/geom_vert_spv.h"
#include "shaders/geom_frag_spv.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr SDL_GPUTextureFormat kColorFmt = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
constexpr SDL_GPUTextureFormat kDepthFmt = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

SDL_GPUDevice*         g_dev   = nullptr;
SDL_GPUTexture*        g_color = nullptr;   // offscreen EFB-sized colour target
SDL_GPUTexture*        g_depth = nullptr;   // depth target
SDL_GPUTransferBuffer* g_dl    = nullptr;   // download staging (w*h*4)
int  g_w = 0, g_h = 0;
bool g_tried = false, g_ok = false;

std::vector<uint8_t> g_cpu;   // last frame read back, top-left origin RGBA8

// Geometry path (milestone 1). Vertices arrive in CLIP space already (the frontend does posMtx +
// projection on the CPU), so one pipeline serves every draw for now — no per-material state yet.
SDL_GPUGraphicsPipeline* g_pipe = nullptr;
SDL_GPUBuffer*           g_vbuf = nullptr;
SDL_GPUTransferBuffer*   g_vup  = nullptr;
size_t                   g_vcap = 0;
std::vector<SbrVertex>   g_verts;   // accumulated this frame

SDL_GPUShader* make_shader(const void* code, size_t bytes, SDL_GPUShaderStage stage) {
    SDL_GPUShaderCreateInfo ci{};
    ci.code = (const Uint8*)code;
    ci.code_size = bytes;
    ci.entrypoint = "main";
    ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
    ci.stage = stage;
    return SDL_CreateGPUShader(g_dev, &ci);
}

void ensure_vbuf(size_t bytes) {
    if (bytes <= g_vcap && g_vbuf != nullptr) return;
    if (g_vbuf) SDL_ReleaseGPUBuffer(g_dev, g_vbuf);
    if (g_vup)  SDL_ReleaseGPUTransferBuffer(g_dev, g_vup);
    g_vcap = bytes + (bytes / 2) + 4096;
    SDL_GPUBufferCreateInfo bci{};
    bci.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bci.size = (Uint32)g_vcap;
    g_vbuf = SDL_CreateGPUBuffer(g_dev, &bci);
    SDL_GPUTransferBufferCreateInfo tbci{};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size = (Uint32)g_vcap;
    g_vup = SDL_CreateGPUTransferBuffer(g_dev, &tbci);
}

bool enabled() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SBR_SDLGPU");
        v = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    return v == 1;
}

} // namespace

bool sbr_render_enabled() { return enabled(); }

bool sbr_render_init(int w, int h) {
    if (g_tried) return g_ok && g_w == w && g_h == h;
    g_tried = true;

    // Aurora has already SDL_Init'd video (it owns the window); this reuses that. SDL3 GPU runs its
    // own Vulkan instance independent of aurora's Dawn, so the two devices coexist in the process.
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        lucent::error("nrender", "SDL_InitSubSystem(VIDEO) failed: {}", SDL_GetError());
        return false;
    }
    g_dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, /*debug=*/true, nullptr);
    if (g_dev == nullptr) {
        lucent::error("nrender", "SDL_CreateGPUDevice failed: {}", SDL_GetError());
        return false;
    }

    SDL_GPUTextureCreateInfo cci{};
    cci.type = SDL_GPU_TEXTURETYPE_2D;
    cci.format = kColorFmt;
    cci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    cci.width = (Uint32)w;
    cci.height = (Uint32)h;
    cci.layer_count_or_depth = 1;
    cci.num_levels = 1;
    cci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    g_color = SDL_CreateGPUTexture(g_dev, &cci);

    SDL_GPUTextureCreateInfo dci = cci;
    dci.format = kDepthFmt;
    dci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    g_depth = SDL_CreateGPUTexture(g_dev, &dci);

    if (g_color == nullptr || g_depth == nullptr) {
        lucent::error("nrender", "target texture create failed: {}", SDL_GetError());
        return false;
    }

    SDL_GPUTransferBufferCreateInfo tbci{};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbci.size = (Uint32)(w * h * 4);
    g_dl = SDL_CreateGPUTransferBuffer(g_dev, &tbci);

    // One pipeline for milestone 1: pass-through clip-space verts, no depth test yet (the GX z
    // modes come with the state machine), alpha blending off. Everything about it is provisional
    // and becomes per-material state as the TEV milestone lands.
    SDL_GPUShader* vs = make_shader(kGeomVertSpv, sizeof(kGeomVertSpv), SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader* fs = make_shader(kGeomFragSpv, sizeof(kGeomFragSpv), SDL_GPU_SHADERSTAGE_FRAGMENT);
    if (vs == nullptr || fs == nullptr) {
        lucent::error("nrender", "shader create failed: {}", SDL_GetError());
        return false;
    }
    SDL_GPUVertexBufferDescription vbd{};
    vbd.slot = 0;
    vbd.pitch = sizeof(SbrVertex);
    vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    SDL_GPUVertexAttribute va[2]{};
    va[0].location = 0; va[0].buffer_slot = 0;
    va[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; va[0].offset = 0;
    va[1].location = 1; va[1].buffer_slot = 0;
    va[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; va[1].offset = 16;

    SDL_GPUColorTargetDescription ctd{};
    ctd.format = kColorFmt;

    SDL_GPUGraphicsPipelineCreateInfo pci{};
    pci.vertex_shader = vs;
    pci.fragment_shader = fs;
    pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pci.vertex_input_state.vertex_buffer_descriptions = &vbd;
    pci.vertex_input_state.num_vertex_buffers = 1;
    pci.vertex_input_state.vertex_attributes = va;
    pci.vertex_input_state.num_vertex_attributes = 2;
    pci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;   // GX cull comes with the state machine
    pci.target_info.color_target_descriptions = &ctd;
    pci.target_info.num_color_targets = 1;
    pci.target_info.depth_stencil_format = kDepthFmt;
    pci.target_info.has_depth_stencil_target = true;
    g_pipe = SDL_CreateGPUGraphicsPipeline(g_dev, &pci);
    SDL_ReleaseGPUShader(g_dev, vs);
    SDL_ReleaseGPUShader(g_dev, fs);
    if (g_pipe == nullptr) {
        lucent::error("nrender", "pipeline create failed: {}", SDL_GetError());
        return false;
    }

    g_cpu.assign((size_t)w * h * 4, 0);
    g_w = w;
    g_h = h;
    g_ok = true;
    lucent::info("nrender", "SDL3 GPU device up ({}x{}), driver={}", w, h,
                 SDL_GetGPUDeviceDriver(g_dev));
    return true;
}

namespace { SDL_FColor g_clear{}; int g_lastVerts = 0; }

void sbr_render_begin(float r, float g, float b, float a) {
    if (!g_ok) return;
    g_clear = SDL_FColor{r, g, b, a};
    g_verts.clear();
}

void sbr_render_tris(const SbrVertex* verts, int count) {
    if (!g_ok || verts == nullptr || count < 3) return;
    count -= count % 3;
    g_verts.insert(g_verts.end(), verts, verts + count);
}

// Upload the frame's geometry, render it in one pass over a cleared target, then download for
// readback / the A/B against aurora. The whole sequence — acquire, copy pass, render pass, copy
// pass, fence, map — is the shape every later milestone keeps; only what goes INSIDE the render
// pass grows (per-material pipelines, textures, TEV).
void sbr_render_end() {
    if (!g_ok) return;
    g_lastVerts = (int)g_verts.size();

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    if (cmd == nullptr) {
        lucent::error("nrender", "AcquireGPUCommandBuffer failed: {}", SDL_GetError());
        return;
    }

    const size_t vbytes = g_verts.size() * sizeof(SbrVertex);
    if (vbytes > 0) {
        ensure_vbuf(vbytes);
        if (void* m = SDL_MapGPUTransferBuffer(g_dev, g_vup, false)) {
            std::memcpy(m, g_verts.data(), vbytes);
            SDL_UnmapGPUTransferBuffer(g_dev, g_vup);
        }
        SDL_GPUCopyPass* up = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation src{}; src.transfer_buffer = g_vup; src.offset = 0;
        SDL_GPUBufferRegion dst{}; dst.buffer = g_vbuf; dst.offset = 0; dst.size = (Uint32)vbytes;
        SDL_UploadToGPUBuffer(up, &src, &dst, false);
        SDL_EndGPUCopyPass(up);
    }

    SDL_GPUColorTargetInfo cti{};
    cti.texture = g_color;
    cti.clear_color = g_clear;
    cti.load_op = SDL_GPU_LOADOP_CLEAR;
    cti.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPUDepthStencilTargetInfo dsi{};
    dsi.texture = g_depth;
    dsi.clear_depth = 1.0f;
    dsi.load_op = SDL_GPU_LOADOP_CLEAR;
    dsi.store_op = SDL_GPU_STOREOP_DONT_CARE;

    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &cti, 1, &dsi);
    if (vbytes > 0) {
        SDL_BindGPUGraphicsPipeline(rp, g_pipe);
        SDL_GPUBufferBinding vb{}; vb.buffer = g_vbuf; vb.offset = 0;
        SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
        SDL_DrawGPUPrimitives(rp, (Uint32)g_verts.size(), 1, 0, 0);
    }
    SDL_EndGPURenderPass(rp);

    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion reg{};
    reg.texture = g_color; reg.w = (Uint32)g_w; reg.h = (Uint32)g_h; reg.d = 1;
    SDL_GPUTextureTransferInfo tti{};
    tti.transfer_buffer = g_dl; tti.pixels_per_row = (Uint32)g_w; tti.rows_per_layer = (Uint32)g_h;
    SDL_DownloadFromGPUTexture(cp, &reg, &tti);
    SDL_EndGPUCopyPass(cp);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence != nullptr) {
        SDL_WaitForGPUFences(g_dev, true, &fence, 1);
        SDL_ReleaseGPUFence(g_dev, fence);
    }
    if (void* mapped = SDL_MapGPUTransferBuffer(g_dev, g_dl, false)) {
        std::memcpy(g_cpu.data(), mapped, g_cpu.size());
        SDL_UnmapGPUTransferBuffer(g_dev, g_dl);
    }
}

int sbr_render_last_vertex_count() { return g_lastVerts; }

bool sbr_render_readback(uint8_t* rgba, int w, int h) {
    if (!g_ok || w != g_w || h != g_h || rgba == nullptr) return false;
    std::memcpy(rgba, g_cpu.data(), (size_t)w * h * 4);
    return true;
}
