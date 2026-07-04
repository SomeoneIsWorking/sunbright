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
#include <dirent.h>
#include <execinfo.h>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

extern TApplication gpApplication;

// JKRHeap host-vs-JKR routing (reference/sms/src/JSystem/JKernel/JKRHeap.cpp):
// plain `new` on a thread that is NOT marked as a game thread falls back to
// host malloc, bypassing the JKR heap entirely. The game thread MUST be marked
// or the game's own heap children (spGameHeapBlock etc.) land outside the root
// arena and JKRGetRootHeap()->getSize() returns -1 for later reads.
extern "C" void sb_mark_game_thread(void);
extern "C" void sb_unmark_game_thread(void);

// ---- Watchdog: SIGALRM handler dumps backtrace and aborts if the boot loop
// stalls. Kick sb_watchdog_kick() from progress checkpoints to reset the timer.
// SB_WATCHDOG_SECS overrides the default (5 s).
static constexpr unsigned kWatchdogDefaultSecs = 5;
// SIGUSR1 handler: each thread that receives it dumps ITS OWN userspace bt.
// The watchdog broadcasts SIGUSR1 to every task tid so we get all threads.
#include <sys/syscall.h>
static void sb_sigusr1_handler(int) {
    pid_t tid = (pid_t)syscall(SYS_gettid);
    dprintf(2, "\n--- Thread tid=%d ---\n", (int)tid);
    void* frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, 2);
}
static void sb_watchdog_handler(int) {
    const char* msg = "\n=== WATCHDOG: no progress within timeout, dumping all threads ===\n";
    write(2, msg, std::strlen(msg));

    pid_t self_tid = (pid_t)syscall(SYS_gettid);
    // First the signal-recipient thread (main) — its own bt is via backtrace().
    dprintf(2, "\n--- Signal-recipient thread (main tid=%d) ---\n", (int)self_tid);
    void* frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, 2);

    // Broadcast SIGUSR1 to every other thread; each dumps its own bt.
    DIR* d = opendir("/proc/self/task");
    if (d) {
        struct dirent* de;
        pid_t pid = getpid();
        while ((de = readdir(d)) != nullptr) {
            if (de->d_name[0] == '.') continue;
            pid_t tid = (pid_t)atoi(de->d_name);
            if (tid == self_tid) continue;
            syscall(SYS_tgkill, pid, tid, SIGUSR1);
        }
        closedir(d);
    }
    // Give the handlers time to run before we exit.
    struct timespec ts = { 0, 200 * 1000 * 1000 };
    nanosleep(&ts, nullptr);
    write(2, "\n=== end thread dump ===\n", 25);
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
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, nullptr);

    struct sigaction su{};
    su.sa_handler = sb_sigusr1_handler;
    sigemptyset(&su.sa_mask);
    su.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &su, nullptr);

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
    // Boost MEM1 well past the GC's 24 MB. On PC there's no reason to
    // pretend we're memory-constrained; the game fills the JKRSolidHeap
    // that ate ~14 MB in scene setup (mesh + shape packets + matrices)
    // and OOMs on the FIRST J3D shape draw with only 24 MB.
    config.mem1Size       = 256 * 1024 * 1024;
    config.mem2Size       = 64 * 1024 * 1024;

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
        sb_mark_game_thread();  // routes plain `new` onto the JKR heap
        gpApplication.initialize();
        gpApplication.proc();
        gpApplication.finalize();
        game_rc.store(0);
    });
    // Main thread is IO-only; its `new` (SDL/Aurora/etc.) must stay off the
    // non-thread-safe JKR heap. The static-init heap bringup may have marked
    // process-main by default; unmark it here.
    sb_unmark_game_thread();

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
