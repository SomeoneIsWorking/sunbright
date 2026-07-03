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
#include "UICommon/UICommon.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

// Once-per-process init logging so the user sees the oracle sink is active.
bool s_announced = false;
bool s_video_up = false;
bool s_video_tried = false;

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

    const bool ok = g_video_backend->Initialize(wsi);
    if (!ok) {
        std::fprintf(stderr, "[oracle] VideoBackend::Initialize returned false — check Dolphin's PanicAlertFmt output above\n");
        return false;
    }
    std::fprintf(stderr, "[oracle] Dolphin video backend UP (headless). Real rendering wires in step 4b.\n");
    s_video_up = true;
    return true;
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
    if (!s_announced) {
        s_announced = true;
        std::fprintf(stderr, "[oracle] sink active\n");
        try_init_video_backend();  // one-shot probe; sets s_video_up
    }

    int w = 0, h = 0;
    sb::gxsdl::backbuffer_size(&w, &h);
    if (w <= 0 || h <= 0) return;  // window/backbuffer not up yet

    static std::vector<uint8_t> s_buf;
    const size_t need = (size_t)w * h * 4;
    if (s_buf.size() != need) s_buf.assign(need, 0);

    paint_stub(s_buf, w, h);

    sb::gxsdl::inject_cpu_frame(s_buf.data(), w, h);

    // NOTE: the NATIVE_PC path also drains sb_boot_capture_tev_take + the imm
    // capture buffers every present (otherwise they grow unbounded). This stub
    // does NOT drain — the buffers stay untouched under GX_ORACLE. Once the
    // Dolphin sink actually consumes captured state we'll drain here; for now,
    // running under SB_RENDER=oracle for long sessions will leak the capture
    // buffers, so treat this stub as a smoke-test-only mode.
}
