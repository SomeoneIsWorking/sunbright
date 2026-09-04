// main.cpp — sms-boot entry point. ONE runtime, ONE thread.
//
// The game (decomp/sms decomp, SMS_NATIVE_PLATFORM=1) runs on the process
// main thread — no game std::thread, no OSThread emulation, no cooperative
// scheduler. Aurora is an IO library called from inside the game's own frame:
// the per-frame present/pump/pace seam is sb_frame_present(), reached from
// JDrama::TVideo::waitForRetrace (see runtime/frame_seam.cpp). I/O is
// synchronous and unthrottled: DVD reads (aurora dvd, nod) complete inline
// with their callbacks fired before the call returns; SZS decode and ARAM
// copies run at the enqueue site. The only threads in the process are
// library-internal (SDL audio device, Dawn/driver workers) — none execute
// game or runtime logic.

#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/main.h>
#include <dolphin/os.h>
#include <dolphin/pad.h>

#include <System/Application.hpp>

#include "runtime/config.h"
#include "runtime/semantic_render.h"

#include <sb_log.h>

#include <cstdlib>
#include <string>

extern TApplication gpApplication;

// FIFO parity harness (SB_FIFO_REPLAY=<path.dff>): replay a captured Dolphin GX
// FIFO through aurora and dump the frame(s), instead of booting the game.
extern "C" int sb_fifo_replay_run(const char* dffPath); // runtime/fifo_player.cpp

// JKRHeap host-vs-JKR routing (decomp/sms/src/JSystem/JKernel/JKRHeap.cpp):
// plain `new` on a thread not marked as THE game thread falls back to host
// malloc. The main thread is marked just before game code starts; host-side
// work inside the frame/DVD seams runs under sb_host_alloc_push/pop.
extern "C" void sb_mark_game_thread(void);
extern "C" void sb_host_alloc_push(void);
extern "C" void sb_host_alloc_pop(void);
extern "C" void sb_watchdog_install(void);
extern "C" void sb_frame_seam_configure(void);
extern "C" void sb_frame_seam_start(void);
extern "C" void sb_pad_script_install(void); // headless scripted input (SB_PAD_SCRIPT)

static void log_callback(AuroraLogLevel level, const char* module, const char* message,
                         unsigned int) {
    switch (level) {
    case LOG_DEBUG:
        sb_logf("aurora", "%s: %s", module, message);
        break;
    case LOG_INFO:
        sb_infof("aurora", "%s: %s", module, message);
        break;
    case LOG_WARNING:
        sb_warnf("aurora", "%s: %s", module, message);
        break;
    case LOG_ERROR:
        sb_errorf("aurora", "%s: %s", module, message);
        break;
    case LOG_FATAL:
        sb_errorf("aurora", "%s: %s", module, message);
        std::abort();
        break;
    default:
        sb_warnf("aurora", "unknown level from %s: %s", module, message);
        break;
    }
}

int main(int argc, char* argv[]) {
    sb_log_configure(nullptr);
    std::string configError;
    if (!sb::configure_runtime(configError)) {
        sb_errorf("config", "%s", configError.c_str());
        return 1;
    }
    const auto& runtimeConfig = sb::runtime_config();
    sb_log_configure(runtimeConfig.logChannels.c_str());
    if (!sb_semantic_render_configure()) {
        sb_errorf("semantic", "configuration failed: %s", sb_semantic_render_last_error());
        return 1;
    }
    sb_frame_seam_configure();
    AuroraConfig config = {};
    config.appName = "Sunbright";
    config.desiredBackend = BACKEND_VULKAN;
    config.logCallback = &log_callback;
    config.logLevel = LOG_INFO;
    config.msaa = 1;
    config.vsync = true;
    // Default window size — small enough that first-frame render / swapchain
    // acquire doesn't cost a minute at the default 3200x1975 the aurora hi-DPI
    // fallback picks. Env SB_W / SB_H override. GC native is 640x480; give it
    // 2x for a bit of headroom while staying cheap.
    config.windowWidth = runtimeConfig.windowWidth;
    config.windowHeight = runtimeConfig.windowHeight;
    // Boost MEM1 well past the GC's 24 MB. On PC there's no reason to
    // pretend we're memory-constrained; the game fills the JKRSolidHeap
    // that ate ~14 MB in scene setup (mesh + shape packets + matrices)
    // and OOMs on the FIRST J3D shape draw with only 24 MB.
    config.mem1Size = 256 * 1024 * 1024;
    config.mem2Size = 64 * 1024 * 1024;

    AuroraInfo info = aurora_initialize(argc, argv, &config);
    sb_infof("startup", "aurora up: backend=%d fb=%ux%u", static_cast<int>(info.backend),
             info.windowSize.fb_width, info.windowSize.fb_height);

    OSInit();

    // FIFO parity harness: if SB_FIFO_REPLAY is set, replay the named .dff
    // through aurora and exit -- no DVD, no game boot. Capture via SB_DUMP_FRAME.
    if (!runtimeConfig.fifoReplayPath.empty()) {
        sb_infof("fifo-replay", "input: %s", runtimeConfig.fifoReplayPath.c_str());
        const int n = sb_fifo_replay_run(runtimeConfig.fifoReplayPath.c_str());
        sb_infof("fifo-replay", "completed: %d frames", n);
        aurora_shutdown();
        return 0;
    }

    if (!aurora_dvd_open(runtimeConfig.romPath.c_str())) {
        sb_errorf("startup", "aurora_dvd_open failed for %s", runtimeConfig.romPath.c_str());
        aurora_shutdown();
        return 1;
    }
    sb_infof("startup", "DVD mounted: %s", runtimeConfig.romPath.c_str());

    if (!sb_semantic_render_initialize(info.window)) {
        sb_errorf("semantic", "initialization failed: %s", sb_semantic_render_last_error());
        aurora_shutdown();
        return 1;
    }

    // Keyboard drives pad 0 unless a physical gamepad takes over.
    PADSetKeyboardActive(0, TRUE);
    // Headless scripted input (SB_PAD_SCRIPT), composed on top via Aurora's
    // virtual-pad seam; no-op when unset.
    sb_pad_script_install();

    sb_watchdog_install();
    sb_frame_seam_start();

    // From here on this is the game thread: plain `new` routes to the JKR
    // heap. The frame/DVD seams gate their host work off it.
    sb_mark_game_thread();
    gpApplication.initialize();
    gpApplication.proc();
    gpApplication.finalize();

    sb_host_alloc_push();
    const bool semanticShutdown = sb_semantic_render_shutdown();
    if (!semanticShutdown) {
        sb_errorf("semantic", "shutdown failed: %s", sb_semantic_render_last_error());
        std::abort();
    }
    aurora_shutdown();
    sb_host_alloc_pop();
    return 0;
}
