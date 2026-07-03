// oracle_present.cpp — GX_ORACLE render sink for sms-boot.
//
// When sb::engine::mode() == GX_ORACLE, sms_boot_present.cpp's VI-present hook
// forwards to sb_oracle_present_frame() here instead of running the SDL3 GPU
// pipeline. The sink is expected to consume the SAME captured GX state /
// geometry the NATIVE_PC sink does (via sb_boot_capture_tev_take,
// sb_imm_take_batch, gx_state.h globals), route it through Dolphin's
// videovulkan backend, and push the resulting frame to the window via
// sb::gxsdl::inject_cpu_frame.
//
// STATUS (Path C step 3): plumbing stub only. The sink currently paints a
// distinctive magenta+diagonal-stripe pattern into the window frame so that
// running `SB_RENDER=oracle build/native/sms-boot` visibly differs from the
// NATIVE_PC sink — proving end-to-end that the runtime toggle takes effect.
// Real Dolphin VideoCommon initialisation + GX-call routing lands in step 4.
//
// This file is ONLY compiled+linked when Dolphin's videovulkan target is
// visible in the CMake scope (see native/CMakeLists.txt: `if(TARGET
// videovulkan)`). A standalone `native/`-only build omits it, and
// sms_boot_present.cpp's dispatch falls back to NATIVE_PC.

#include "gx_sdlgpu.h"

#include "Common/Config/Config.h"
#include "Common/MsgHandler.h"     // SetEnableAlert — silence Y/N prompts
#include "Common/WindowSystemInfo.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"

#include "Common/MathUtil.h"       // Rectangle<int>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

void tame_dolphin_config();

// Once-per-process init logging so the user sees the oracle sink is active.
bool s_announced = false;
bool s_video_up = false;
bool s_video_tried = false;

// Per-frame Dolphin resources, allocated on first successful init.
std::unique_ptr<AbstractTexture> s_color_rt;
std::unique_ptr<AbstractFramebuffer> s_fbo;
std::unique_ptr<AbstractStagingTexture> s_staging;
int s_rt_w = 0, s_rt_h = 0;

bool ensure_rt(int w, int h) {
    if (s_color_rt && s_rt_w == w && s_rt_h == h) return true;
    if (w <= 0 || h <= 0) return false;
    TextureConfig cfg(w, h, 1, 1, 1, AbstractTextureFormat::RGBA8,
                      AbstractTextureFlag_RenderTarget, AbstractTextureType::Texture_2D);
    s_color_rt = g_gfx->CreateTexture(cfg, "oracle_sink_color");
    if (!s_color_rt) {
        std::fprintf(stderr, "[oracle] CreateTexture(color RT) failed\n");
        return false;
    }
    s_fbo = g_gfx->CreateFramebuffer(s_color_rt.get(), nullptr);
    if (!s_fbo) {
        std::fprintf(stderr, "[oracle] CreateFramebuffer failed\n");
        s_color_rt.reset();
        return false;
    }
    s_staging = g_gfx->CreateStagingTexture(StagingTextureType::Readback, cfg);
    if (!s_staging) {
        std::fprintf(stderr, "[oracle] CreateStagingTexture(Readback) failed\n");
        s_fbo.reset(); s_color_rt.reset();
        return false;
    }
    s_rt_w = w; s_rt_h = h;
    std::fprintf(stderr, "[oracle] allocated Dolphin GPU resources: %dx%d RGBA8\n", w, h);
    return true;
}

// Try to bring up Dolphin's video backend once. Returns true on success. Logs
// pass/fail loudly so we can name what's blocking the standalone init on first
// failure (see Path C step 4 notes).
bool try_init_video_backend() {
    if (s_video_tried) return s_video_up;
    s_video_tried = true;
    std::fprintf(stderr, "[oracle] attempting standalone Dolphin video backend init...\n");

    // Silence PanicAlert prompts (Init/backend may hit assertions we want to
    // log-and-continue on, not block on a Y/N stdin prompt).
    Common::SetEnableAlert(false);

    // Point Dolphin at a user directory before Init — otherwise the FS backend
    // panics on empty m_root_path. Isolated per-process to avoid clobbering the
    // oracle build/sunbright's <home>/.config/dolphin-emu. SB_ORACLE_USERDIR overrides.
    if (const char* ud = std::getenv("SB_ORACLE_USERDIR"))
        UICommon::SetUserDirectory(ud);
    else
        UICommon::SetUserDirectory(std::string("./scratch/sms_boot_oracle_userdir"));

    // UICommon::Init brings up the Config layer stack, SConfig, g_Config,
    // LogManager, and calls ActivateBackend(MAIN_GFX_BACKEND). It's the
    // sanctioned entry point (same one runtime/main_sdl.cpp uses) — safer than
    // hand-picking pieces.
    UICommon::Init();
    UICommon::CreateDirectories();

    // Force Vulkan as the graphics backend (BaseConfigLoader may default to
    // OpenGL). PopulateBackendInfo below re-reads MAIN_GFX_BACKEND, so setting
    // this after UICommon::Init is what actually picks Vulkan.
    Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("Vulkan"));

    // Tame Dolphin's threading: sms-boot's game thread runs a cooperative
    // scheduler, and Dolphin's backend worker threads / shader-compile threads
    // race with it (present_hook stops firing after 1 frame with these on).
    tame_dolphin_config();

    // Headless WSI for the first probe — no window, no swapchain. Dolphin's
    // Vulkan backend supports this (see VKMain.cpp: `enable_surface = wsi.type
    // != Headless`). If we get this far without crashing we've proven the
    // instance / physical-device / device / vertex-manager path works
    // standalone; step 4b wires up a real WSI so present goes to the window.
    WindowSystemInfo wsi{};
    wsi.type = WindowSystemType::Headless;

    // PopulateBackendInfo fills g_backend_info (feature flags used during
    // Initialize). It also calls ActivateBackend(config value) — that's why
    // we set MAIN_GFX_BACKEND above.
    VideoBackendBase::PopulateBackendInfo(wsi);

    if (!g_video_backend) {
        std::fprintf(stderr, "[oracle] g_video_backend null after PopulateBackendInfo\n");
        return false;
    }
    std::fprintf(stderr, "[oracle] backend activated: %s\n", g_video_backend->GetDisplayName().c_str());

    std::fprintf(stderr, "[oracle] calling VideoBackend::Initialize...\n");
    std::fflush(stderr);
    const bool ok = g_video_backend->Initialize(wsi);
    std::fprintf(stderr, "[oracle] VideoBackend::Initialize returned %d\n", (int)ok);
    if (!ok) {
        std::fprintf(stderr, "[oracle] VideoBackend::Initialize returned false — check Dolphin's PanicAlertFmt output above\n");
        return false;
    }
    std::fprintf(stderr, "[oracle] Dolphin video backend UP (headless). Real rendering wires in step 4b.\n");
    s_video_up = true;
    std::fflush(stderr);
    return true;
}

// Turn off Dolphin's backend multithreading (spawns a worker thread that races
// with sms-boot's cooperative scheduler). Also disable shader cache disk I/O
// and other threaded features that fight the game's execution model.
void tame_dolphin_config() {
    Config::SetBase(Config::GFX_BACKEND_MULTITHREADING, false);
    Config::SetBase(Config::GFX_SHADER_COMPILATION_MODE,
                    ShaderCompilationMode::Synchronous);
}

// Paint pattern helper — distinctive so a visual diff vs NATIVE_PC is obvious.
void paint_stub(std::vector<uint8_t>& rgba, int w, int h) {
    // Magenta base + white diagonal stripes every 32 px. Frame counter tints the
    // top strip so a static screen still animates (proves per-frame invocation).
    static int s_tick = 0;
    ++s_tick;
    const uint8_t tint = (uint8_t)((s_tick * 7) & 0xff);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = rgba.data() + ((size_t)y * w + x) * 4;
            const bool stripe = ((x + y) / 32) & 1;
            p[0] = stripe ? 0xff : 0xff;       // R
            p[1] = stripe ? 0xff : 0x20;       // G — magenta base, white stripe
            p[2] = stripe ? 0xff : 0xff;       // B
            p[3] = 0xff;                        // A
            if (y < 16) p[0] = tint;            // top strip: animating tick
        }
    }
}

} // namespace

extern "C" void sb_oracle_present_frame(void* /*framebuffer*/, void* /*user*/) {
    static long s_beat = 0;
    ++s_beat;
    if ((s_beat % 60) == 1) std::fprintf(stderr, "[oracle-beat] calls=%ld\n", s_beat);
    static const bool skip_init = [](){ const char* v = std::getenv("SB_ORACLE_SKIP_INIT"); return v && v[0] && v[0] != '0'; }();
    if (!s_announced) {
        s_announced = true;
        std::fprintf(stderr, "[oracle] sink active\n");
        if (!skip_init) try_init_video_backend();  // one-shot probe; sets s_video_up
    }

    // Fixed render target size. sms-boot's SDL3 backbuffer is 0x0 when SB_WINDOW=0
    // (headless — the default); ignore it and use a native 640x480 which matches
    // the GC XFB. The result gets both inject_cpu_frame'd (if a window exists) and
    // PPM-dumped so a headless run still produces verifiable output.
    int w = 640, h = 480;
    int win_w = 0, win_h = 0;
    sb::gxsdl::backbuffer_size(&win_w, &win_h);
    if (win_w > 0 && win_h > 0) { w = win_w; h = win_h; }

    // Fallback: Dolphin didn't come up — paint stub so the user still sees
    // something diagnostic instead of a black window.
    if (!s_video_up || !g_gfx) {
        static std::vector<uint8_t> s_buf;
        const size_t need = (size_t)w * h * 4;
        if (s_buf.size() != need) s_buf.assign(need, 0);
        paint_stub(s_buf, w, h);
        if (win_w > 0) sb::gxsdl::inject_cpu_frame(s_buf.data(), w, h);
        return;
    }

    // Diagnostic: is present_hook itself being called repeatedly? Skip all
    // Dolphin work every OTHER call to isolate whether Dolphin's GPU submit
    // stalls the game thread. SB_ORACLE_SKIP_DOLPHIN=1 skips all Dolphin work.
    static const bool skip_dolphin = [](){ const char* v = std::getenv("SB_ORACLE_SKIP_DOLPHIN"); return v && v[0] && v[0] != '0'; }();
    if (skip_dolphin) return;

    // Dolphin path (step 4b-A): clear a Dolphin-allocated color RT, read back
    // to CPU, inject into sms-boot's window. Solid color driven by frame
    // counter proves both the GPU pipeline (clear + submit) and the readback
    // path work. Step 4c wires up real GX state → VertexManager → this RT.
    if (!ensure_rt(w, h)) return;

    static int s_frame = 0;
    ++s_frame;
    // Animate the clear so it's visibly Dolphin-driven, not a static paint.
    const float t = (float)(s_frame & 0xff) / 255.0f;
    ClearColor clear{{t, 0.15f, 1.0f - t, 1.0f}};

    g_gfx->BeginUtilityDrawing();
    g_gfx->SetAndClearFramebuffer(s_fbo.get(), clear, 0.0f);

    static std::vector<uint8_t> s_buf;
    const size_t need = (size_t)w * h * 4;
    if (s_buf.size() != need) s_buf.assign(need, 0);

    // Download the just-cleared RT to CPU each frame. Flush() is a synchronous
    // wait for GPU; that's fine on sms-boot's single game thread — no worker
    // thread stall (backend multithreading is off for that reason, see
    // tame_dolphin_config in try_init_video_backend).
    MathUtil::Rectangle<int> full(0, 0, w, h);
    s_staging->CopyFromTexture(s_color_rt.get(), full, 0, 0, full);
    s_staging->Flush();
    s_staging->ReadTexels(full, s_buf.data(), (u32)(w * 4));

    g_gfx->EndUtilityDrawing();
    // PresentBackbuffer cycles per-frame resources (descriptor pools, command
    // buffer ring). Skipping it grows GPU memory unbounded — see Presenter.cpp
    // comment around line 1183.
    g_gfx->PresentBackbuffer();

    if (win_w > 0) sb::gxsdl::inject_cpu_frame(s_buf.data(), w, h);

    // Dump a PPM for the first few frames so headless runs produce visible
    // evidence Dolphin rendered. Path C step 4b-A verification.
    if (s_frame <= 5) {
        char path[256];
        std::snprintf(path, sizeof(path), "scratch/frames/oracle_%04d.ppm", s_frame);
        if (FILE* f = std::fopen(path, "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", w, h);
            // s_buf is RGBA — write RGB triplets.
            std::vector<uint8_t> rgb((size_t)w * h * 3);
            for (int i = 0; i < w * h; ++i) {
                rgb[i * 3 + 0] = s_buf[i * 4 + 0];
                rgb[i * 3 + 1] = s_buf[i * 4 + 1];
                rgb[i * 3 + 2] = s_buf[i * 4 + 2];
            }
            std::fwrite(rgb.data(), 1, rgb.size(), f);
            std::fclose(f);
            std::fprintf(stderr, "[oracle] wrote %s\n", path);
        }
    }

    // NOTE: the NATIVE_PC path also drains sb_boot_capture_tev_take + the imm
    // capture buffers every present. Not yet drained here — step 4c wires the
    // captured GX state into Dolphin's pipeline, at which point that drain
    // becomes the input to the Dolphin renderer instead of unused work.
}
