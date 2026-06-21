// watchdog_impl.cpp — a stuck-process watchdog for the native bring-up.
//
// The cooperative single-runner scheduler (os_impl.cpp) can wedge in two ways: a
// genuine DEADLOCK (no runnable thread — that one already OSPanics in hand_baton),
// or a livelock / infinite loop in game code that makes no forward progress. This
// watchdog catches the latter: a monitor host thread watches a heartbeat that is
// kicked at every scheduler hand-off and every VI retrace. If the heartbeat stops
// advancing for SB_WATCHDOG_SECS (default 20), the watchdog signals the
// currently-running fiber, whose SIGUSR1 handler prints a backtrace of exactly
// where it is stuck, then hard-exits.
//
// The monitor is a PURE host thread: it never calls an OS seam primitive, never
// takes the cooperative baton, and only touches atomics — so it does not perturb
// the "no real concurrency" invariant.
//
// Env: SB_WATCHDOG_SECS=<n>  timeout in seconds (default 20; 0 disables).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <pthread.h>
#include <thread>
#include <unistd.h>

namespace {

std::atomic<unsigned long long> g_heartbeat{0};
std::atomic<unsigned long>      g_run_pth{0};   // pthread_t of the running fiber
std::atomic<bool>               g_started{false};
std::atomic<bool>               g_fired{false};
int                             g_timeout_secs = 20;

// SIGUSR1 handler — runs ON the stuck fiber, so its backtrace is the stuck stack.
// Uses only async-signal-safe output (backtrace_symbols_fd writes straight to fd 2).
void bt_handler(int /*sig*/) {
    void* frames[80];
    int n = backtrace(frames, 80);
    static const char hdr[] =
        "\n=== WATCHDOG: process STUCK (no forward progress) — running-thread backtrace ===\n";
    ssize_t w = write(2, hdr, sizeof(hdr) - 1); (void)w;
    backtrace_symbols_fd(frames, n, 2);
    static const char ftr[] = "=== WATCHDOG: exiting (134) ===\n";
    w = write(2, ftr, sizeof(ftr) - 1); (void)w;
    _exit(134);
}

void monitor_main() {
    unsigned long long last = g_heartbeat.load();
    int stalled = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        unsigned long long now = g_heartbeat.load();
        if (now != last) { last = now; stalled = 0; continue; }
        if (++stalled < g_timeout_secs) continue;
        if (g_fired.exchange(true)) return;
        unsigned long pth = g_run_pth.load();
        std::fprintf(stderr,
                     "\n[watchdog] no forward progress for %ds — signalling running thread\n",
                     stalled);
        std::fflush(stderr);
        if (pth) pthread_kill((pthread_t)pth, SIGUSR1);
        // Grace period for the handler to print + exit; otherwise hard-kill so a
        // wedged signal path can never leave the process hung forever.
        std::this_thread::sleep_for(std::chrono::seconds(3));
        static const char m[] = "[watchdog] handler did not complete — hard exit\n";
        ssize_t w = write(2, m, sizeof(m) - 1); (void)w;
        _exit(134);
    }
}

} // namespace

extern "C" {

// Record the calling thread as the currently-running fiber (so the watchdog signals
// the right one). Called by the scheduler whenever a fiber takes the baton.
void sb_watchdog_set_running(void) {
    g_run_pth.store((unsigned long)pthread_self());
}

// Forward-progress heartbeat — called at every scheduler hand-off + VI retrace.
void sb_watchdog_kick(void) {
    g_heartbeat.fetch_add(1, std::memory_order_relaxed);
}

// Start the monitor (idempotent). Call once from boot after PlatformInit, NOT while
// holding g_sched (it constructs a std::thread whose alloc routes through the heap).
void sb_watchdog_init(void) {
    if (g_started.exchange(true)) return;
    const char* e = std::getenv("SB_WATCHDOG_SECS");
    if (e && *e) {
        int v = std::atoi(e);
        if (v <= 0) return;          // SB_WATCHDOG_SECS=0 (or invalid) disables
        g_timeout_secs = v;
    }
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = bt_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, nullptr);
    sb_watchdog_set_running();       // assume the caller (boot main) is running now
    std::thread(monitor_main).detach();
    std::fprintf(stderr, "[watchdog] armed (timeout %ds)\n", g_timeout_secs);
}

} // extern "C"
