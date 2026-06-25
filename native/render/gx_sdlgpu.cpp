// gx_sdlgpu.cpp — see gx_sdlgpu.h. P1 of the GX→SDL3-GPU switch: headless GPU device + offscreen
// color target + clear + readback. Geometry (P2) and the TEV combiner (P3) extend draw_tev later.
// docs/gx_sdlgpu_switch.md.
#include "gx_sdlgpu.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>

namespace sb::gxsdl {

namespace {
bool  g_init_tried = false;
bool  g_ok         = false;
int   g_w = 0, g_h = 0;

SDL_GPUDevice*         g_dev  = nullptr;
SDL_GPUTexture*        g_color = nullptr;   // offscreen color target (EFB-sized)
SDL_GPUTransferBuffer* g_xfer = nullptr;    // download buffer (w*h*4)

// Per-frame command recording state.
SDL_GPUCommandBuffer*  g_cmd = nullptr;
bool                   g_in_frame = false;

// CPU copy of the last downloaded frame (top-left origin, RGBA8).
std::vector<uint8_t>   g_cpu;
}

bool enabled() {
    static int v = -1;
    if (v < 0) { const char* e = std::getenv("SB_SDLGPU"); v = (e && e[0] && e[0] != '0') ? 1 : 0; }
    return v == 1;
}

bool init(int w, int h) {
    if (g_init_tried) return g_ok && g_w == w && g_h == h;
    g_init_tried = true;

    // SDL3 GPU needs the video subsystem even windowless. For headless (no DISPLAY/Wayland) pick the
    // offscreen video driver so device creation doesn't need a display surface.
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY") && !std::getenv("SDL_VIDEODRIVER"))
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "[gxsdl] SDL_InitSubSystem(VIDEO) failed: %s\n", SDL_GetError());
        return false;
    }

    g_dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, /*debug=*/true, nullptr);
    if (!g_dev) { std::fprintf(stderr, "[gxsdl] SDL_CreateGPUDevice failed: %s\n", SDL_GetError()); return false; }

    SDL_GPUTextureCreateInfo tci{};
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tci.width = (Uint32)w; tci.height = (Uint32)h;
    tci.layer_count_or_depth = 1; tci.num_levels = 1; tci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    g_color = SDL_CreateGPUTexture(g_dev, &tci);
    if (!g_color) { std::fprintf(stderr, "[gxsdl] CreateGPUTexture(color) failed: %s\n", SDL_GetError()); return false; }

    SDL_GPUTransferBufferCreateInfo tbci{};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbci.size = (Uint32)(w * h * 4);
    g_xfer = SDL_CreateGPUTransferBuffer(g_dev, &tbci);
    if (!g_xfer) { std::fprintf(stderr, "[gxsdl] CreateGPUTransferBuffer failed: %s\n", SDL_GetError()); return false; }

    g_cpu.assign((size_t)w * h * 4, 0);
    g_w = w; g_h = h; g_ok = true;
    std::fprintf(stderr, "[gxsdl] SDL3 GPU device up (%dx%d), driver=%s\n", w, h, SDL_GetGPUDeviceDriver(g_dev));
    return true;
}

void frame_begin(float r, float g, float b, float a) {
    if (!g_ok || g_in_frame) return;
    g_cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    if (!g_cmd) { std::fprintf(stderr, "[gxsdl] AcquireGPUCommandBuffer failed: %s\n", SDL_GetError()); return; }

    // P1: clear the color target. A render pass with LOADOP_CLEAR + no draws yields the clear colour.
    SDL_GPUColorTargetInfo cti{};
    cti.texture = g_color;
    cti.clear_color = SDL_FColor{ r, g, b, a };
    cti.load_op = SDL_GPU_LOADOP_CLEAR;
    cti.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(g_cmd, &cti, 1, nullptr);
    // (P2+ records draws into rp here.)
    SDL_EndGPURenderPass(rp);
    g_in_frame = true;
}

void frame_end() {
    if (!g_ok || !g_in_frame || !g_cmd) { g_in_frame = false; return; }

    // Copy pass: download the rendered color texture into the transfer buffer.
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(g_cmd);
    SDL_GPUTextureRegion reg{}; reg.texture = g_color; reg.w = (Uint32)g_w; reg.h = (Uint32)g_h; reg.d = 1;
    SDL_GPUTextureTransferInfo tti{}; tti.transfer_buffer = g_xfer;
    tti.pixels_per_row = (Uint32)g_w; tti.rows_per_layer = (Uint32)g_h;
    SDL_DownloadFromGPUTexture(cp, &reg, &tti);
    SDL_EndGPUCopyPass(cp);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(g_cmd);
    if (fence) { SDL_WaitForGPUFences(g_dev, true, &fence, 1); SDL_ReleaseGPUFence(g_dev, fence); }
    g_cmd = nullptr;

    void* mapped = SDL_MapGPUTransferBuffer(g_dev, g_xfer, false);
    if (mapped) { std::memcpy(g_cpu.data(), mapped, g_cpu.size()); SDL_UnmapGPUTransferBuffer(g_dev, g_xfer); }
    g_in_frame = false;
}

void draw_tev(const sb::render::NvkTevVertex*, int,
              const sb::render::Nvk::NvkTevBatch*, int) {
    // P1 stub — geometry replay arrives in P2 (docs/gx_sdlgpu_switch.md).
}

bool readback(uint8_t* rgba, int w, int h) {
    if (!g_ok || w != g_w || h != g_h || !rgba) return false;
    std::memcpy(rgba, g_cpu.data(), (size_t)w * h * 4);   // SDL3 GPU textures are top-left origin
    return true;
}

} // namespace sb::gxsdl
