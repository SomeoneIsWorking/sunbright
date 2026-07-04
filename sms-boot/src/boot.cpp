// boot.cpp — the native PC bring-up entry. The game's own int main() (reference/sms/
// src/main.cpp: gpApplication.initialize/proc/finalize) is THE program main; this wraps
// it (linker --wrap=main) so the platform layer comes up first — the PC replacement for
// the GameCube __start that ran PlatformInit-equivalent before main.
//
// Links sms-native (the game logic) + sms-platform (the SDK seams). Until the remaining
// seams + game C++ stubs resolve this WON'T link; the undefined-reference list is the
// true critical-path stub set to fill scene-by-scene (handoff step 3/boot).
#include <cstdio>
#include <cstdlib>
#include "../platform/vi_present.h"  // sb_vi_set_pacing (SB_TURBO)
#include <thread>
#include <SDL3/SDL.h>                 // SDL_Delay (main-thread window loop)
#include "../render/gx_sdlgpu.h"      // sb::gxsdl main-thread present
#include "../../runtime/engine.h"     // sb::engine::init_from_env — render-sink toggle

namespace sb::platform { bool PlatformInit(int argc, char** argv); void PlatformShutdown(); }

extern "C" int __real_main(int argc, char** argv);
extern "C" void sb_watchdog_init(void);  // stuck-process watchdog (watchdog_impl.cpp)
extern "C" void sb_pad_driver_install(void);  // headless scripted input (pad_driver.cpp)
extern "C" void sb_sched_retire_current_fiber(void);  // os_impl.cpp — drop process-main from the scheduler
extern "C" void sb_mark_game_thread(void);    // JKRHeap.cpp — this host thread may alloc on the JKR heap
extern "C" void sb_unmark_game_thread(void);  // JKRHeap.cpp — this host thread must NOT touch the JKR heap

// native/render/sms_boot_present.cpp — installs the VI present hook (nvk render + frame dump).
extern "C" void sb_boot_present_install();

extern "C" int __wrap_main(int argc, char** argv) {
    std::printf("[boot] native SMS bring-up: PlatformInit...\n");
    // Resolve render sink from SB_RENDER before anything else — later boot code
    // may query mode() when deciding which pipeline to initialize.
    sb::engine::init_from_env();
    if (!sb::platform::PlatformInit(argc, argv)) {
        std::fprintf(stderr, "[boot] PlatformInit failed (no disc? set SUNBRIGHT_ROM)\n");
        return 1;
    }
    sb_boot_present_install();
    sb_watchdog_init();  // arm the stuck-process watchdog (SB_WATCHDOG_SECS, 0=off)
    if (const char* t = std::getenv("SB_TURBO"); t && t[0] && t[0] != '0')
        // Run uncapped (no VI pacing). DIAGNOSTIC ONLY — disabling pacing changes the
        // cooperative scheduler's yield timing, which can race async asset loads through
        // the boot->gameplay transition (e.g. TCardLoad::titleDraw reading panes before
        // load() populates them). The real-time path is unaffected. Use for post-boot
        // iteration speed, not as the verification path.
        sb_vi_set_pacing(false);
    sb_pad_driver_install();  // headless scripted input, if SB_PAD_SCRIPT/SB_PAD_DRIVE set

    // SB_HARNESS=<test-name>: Tier-1 vs Tier-2 in-process parity harness (task #29,
    // 2026-07-04 direction pivot). Skips the game entirely; runs a hand-authored
    // synthetic render sequence through the active tier sink and dumps a
    // deterministic PPM to scratch/parity/<test>.<tier>.ppm. Compares via
    // tools/render/tier_parity.sh. See native/src/render_parity.cpp.
    if (const char* harness = std::getenv("SB_HARNESS"); harness && harness[0]) {
        extern int sb_render_parity_run(const char*);
        int rc = sb_render_parity_run(harness);
        sb::platform::PlatformShutdown();
        return rc;
    }

    std::printf("[boot] PlatformInit OK -> game main()\n");

    // ── ONE boot path (PC-port threading model) ──────────────────────────────────────────────────
    // The GAME always runs on its own thread; the PROCESS-MAIN thread always owns windowing/present
    // and the SDL event pump. There is no second code path: "window vs headless" is only whether a
    // window is shown (window_preinit/present_window are no-ops otherwise) and "turbo vs real-time"
    // is only pacing (handled above). This is the deliberate two-thread top-level structure (SDL main
    // + one game thread); the game's OSThread async-loaders are run synchronously by the cooperative
    // scheduler and are NOT part of this split. Presenting must happen on the SDL main thread —
    // driving X11/Vulkan-WSI from the game/scheduler thread crashes/deadlocks (see gx_sdlgpu.cpp).
    const bool windowed = sb::gxsdl::window_mode();
    if (windowed)
        sb::gxsdl::window_preinit();   // SDL_Init(VIDEO) on THIS (main) thread before the game starts
    static int s_rc = 0;
    std::thread game([argc, argv]() {
        // The game thread owns the JKR heap (objects the engine `new`s must land there, not host
        // malloc) — mark it, exactly as OSCreateThread's carriers are marked. Without this, game
        // objects split across two allocators and pointers read back as garbage (getNameRef SIGSEGV).
        sb_mark_game_thread();
        s_rc = __real_main(argc, argv);
        sb::gxsdl::mark_game_done();
    });
    // A static-init heap bringup already registered THIS (process-main) thread as the cooperative
    // scheduler's "main fiber" AND marked it a game thread. Now that the real game runs on its own
    // thread, (1) retire process-main from the scheduler so it is never handed the baton while it
    // sits in the present/wait loop below (otherwise: lost wakeup -> deadlock), and (2) unmark it as
    // a game thread so its SDL/present allocations use host malloc, never the non-thread-safe JKR heap.
    sb_sched_retire_current_fiber();
    sb_unmark_game_thread();
    // Main-thread present + event loop. Exits when the game finishes OR (windowed) the window closes.
    while (!sb::gxsdl::game_done() && !(windowed && sb::gxsdl::window_should_quit())) {
        if (windowed) sb::gxsdl::present_window();   // pump events + blit g_cpu (window only)
        SDL_Delay(8);                                // ~120 Hz poll; the game paces itself
    }
    if (windowed && sb::gxsdl::window_should_quit()) {
        // User closed the window: the game thread may still be mid-work and parked in the scheduler;
        // we're quitting the process anyway, so don't join — just shut down + exit.
        game.detach();
        sb::platform::PlatformShutdown();
        return 0;
    }
    game.join();
    sb::platform::PlatformShutdown();
    return s_rc;
}
