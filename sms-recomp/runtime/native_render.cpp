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

    g_cpu.assign((size_t)w * h * 4, 0);
    g_w = w;
    g_h = h;
    g_ok = true;
    lucent::info("nrender", "SDL3 GPU device up ({}x{}), driver={}", w, h,
                 SDL_GetGPUDeviceDriver(g_dev));
    return true;
}

// Milestone 0: a render pass that only CLEARS colour+depth to `clear`, then downloads the target.
// This exercises the whole path a real frame will use — acquire, render pass, copy pass, fence,
// map — so once geometry is added the plumbing is already proven.
void sbr_render_clear(float r, float g, float b, float a) {
    if (!g_ok) return;
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    if (cmd == nullptr) {
        lucent::error("nrender", "AcquireGPUCommandBuffer failed: {}", SDL_GetError());
        return;
    }

    SDL_GPUColorTargetInfo cti{};
    cti.texture = g_color;
    cti.clear_color = SDL_FColor{r, g, b, a};
    cti.load_op = SDL_GPU_LOADOP_CLEAR;
    cti.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPUDepthStencilTargetInfo dsi{};
    dsi.texture = g_depth;
    dsi.clear_depth = 1.0f;
    dsi.load_op = SDL_GPU_LOADOP_CLEAR;
    dsi.store_op = SDL_GPU_STOREOP_DONT_CARE;

    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &cti, 1, &dsi);
    // Nothing drawn yet — milestone 0 is the clear itself.
    SDL_EndGPURenderPass(rp);

    // Download the target for readback / A/B against aurora.
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion reg{};
    reg.texture = g_color;
    reg.w = (Uint32)g_w;
    reg.h = (Uint32)g_h;
    reg.d = 1;
    SDL_GPUTextureTransferInfo tti{};
    tti.transfer_buffer = g_dl;
    tti.pixels_per_row = (Uint32)g_w;
    tti.rows_per_layer = (Uint32)g_h;
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

bool sbr_render_readback(uint8_t* rgba, int w, int h) {
    if (!g_ok || w != g_w || h != g_h || rgba == nullptr) return false;
    std::memcpy(rgba, g_cpu.data(), (size_t)w * h * 4);
    return true;
}
