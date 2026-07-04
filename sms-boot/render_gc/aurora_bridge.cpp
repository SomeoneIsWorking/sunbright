// aurora_bridge.cpp — sms-boot's entry point.
//
// PC threading model (per user directive 2026-07-05, mirrors the old Path-B
// boot.cpp): MAIN thread owns the window and pumps Aurora (event loop,
// aurora_begin_frame/end_frame, SDL). GAME runs on a real PC std::thread —
// NOT a GC OSThread — that owns TApplication::initialize/proc/finalize and
// issues GX draws. Aurora is IO-only: input in, pixels out. It never drives
// game logic.

#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <dolphin/gx.h>
#include <dolphin/os.h>

#include <System/Application.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <thread>
#include <unistd.h>

extern TApplication gpApplication;

// ---- Watchdog: SIGALRM handler dumps backtrace and aborts if the boot loop
// stalls. Kick sb_watchdog_kick() from progress checkpoints to reset the timer.
// SB_WATCHDOG_SECS overrides the default (5 s).
static constexpr unsigned kWatchdogDefaultSecs = 5;
static void sb_watchdog_handler(int) {
    const char* msg = "\n=== WATCHDOG: no progress within timeout, aborting ===\n";
    write(2, msg, std::strlen(msg));
    void* frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, 2);
    write(2, "\n", 1);
    _exit(134);
}
extern "C" void sb_watchdog_kick(void) {
    const char* env = std::getenv("SB_WATCHDOG_SECS");
    unsigned secs = env ? (unsigned)std::atoi(env) : kWatchdogDefaultSecs;
    if (secs == 0) secs = kWatchdogDefaultSecs;
    alarm(secs);
}
static void sb_watchdog_install(void) {
    struct sigaction sa{};
    sa.sa_handler = sb_watchdog_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, nullptr);
    sb_watchdog_kick();
}

static void log_callback(AuroraLogLevel level, const char* module,
                         const char* message, unsigned int) {
    const char* tag = "?";
    FILE* out = stdout;
    switch (level) {
        case LOG_DEBUG:   tag = "DEBUG";   break;
        case LOG_INFO:    tag = "INFO";    break;
        case LOG_WARNING: tag = "WARN";    break;
        case LOG_ERROR:   tag = "ERROR";   out = stderr; break;
        case LOG_FATAL:   tag = "FATAL";   out = stderr; break;
    }
    std::fprintf(out, "[aurora %s %s] %s\n", tag, module, message);
    if (level == LOG_FATAL) { std::fflush(out); std::abort(); }
}

int main(int argc, char* argv[]) {
    AuroraConfig config = {};
    config.appName        = "Sunbright";
    config.desiredBackend = BACKEND_VULKAN;
    config.logCallback    = &log_callback;
    config.logLevel       = LOG_INFO;
    config.msaa           = 1;
    config.vsync          = true;
    config.mem1Size       = MEM1_DEFAULT_SIZE;
    config.mem2Size       = ARAM_DEFAULT_SIZE;

    AuroraInfo info = aurora_initialize(argc, argv, &config);
    std::fprintf(stdout, "[sms-boot] aurora up: backend=%d fb=%ux%u\n",
                 (int)info.backend, info.windowSize.fb_width, info.windowSize.fb_height);
    std::fflush(stdout);

    OSInit();

    const char* rom = std::getenv("SUNBRIGHT_ROM");
    if (!rom || !*rom) rom = "rom.rvz";
    if (!aurora_dvd_open(rom)) {
        std::fprintf(stderr, "[sms-boot] aurora_dvd_open failed for %s\n", rom);
        return 1;
    }
    std::fprintf(stdout, "[sms-boot] DVD mounted: %s\n", rom);
    std::fflush(stdout);

    sb_watchdog_install();

    // GAME thread — PC std::thread, NOT a GC OSThread. Owns the game main
    // loop; issues GX draws. Aurora is called only from THIS main thread
    // (window/present/event pump).
    std::atomic<int> game_rc{-1};
    std::thread game([&game_rc]() {
        gpApplication.initialize();
        gpApplication.proc();
        gpApplication.finalize();
        game_rc.store(0);
    });

    // MAIN thread — Aurora IO pump. Runs the event loop and per-frame
    // begin/end so the swapchain doesn't stall. Exits when the game thread
    // finishes or the user closes the window.
    bool exit_requested = false;
    while (!exit_requested && game_rc.load() == -1) {
        const AuroraEvent* event = aurora_update();
        while (event && event->type != AURORA_NONE) {
            if (event->type == AURORA_EXIT) exit_requested = true;
            else if (event->type == AURORA_WINDOW_RESIZED) info.windowSize = event->windowSize;
            ++event;
        }
        if (aurora_begin_frame()) {
            aurora_end_frame();
        }
        // Yield so the game thread can make progress; ~120 Hz poll.
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    if (exit_requested && game_rc.load() == -1) {
        // User asked to quit while game is mid-work: detach and shut down.
        game.detach();
    } else {
        game.join();
    }

    aurora_shutdown();
    return game_rc.load() == -1 ? 0 : game_rc.load();
}
