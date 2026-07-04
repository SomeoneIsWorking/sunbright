// watchdog_impl.cpp — a stuck-process watchdog for the native bring-up.
//
// The cooperative single-runner scheduler (os_impl.cpp) can wedge in two ways: a
// genuine DEADLOCK (no runnable thread — that one already OSPanics in hand_baton),
// or a livelock / infinite loop in game code that makes no forward progress. This
// watchdog catches the latter: a monitor host thread watches a heartbeat that is
// kicked at every scheduler hand-off and every VI retrace. If the heartbeat stops
// advancing for SB_WATCHDOG_SECS (default 8), the watchdog signals the
// currently-running fiber, whose SIGUSR1 handler prints a backtrace of exactly
// where it is stuck, then hard-exits.
//
// The monitor is a PURE host thread: it never calls an OS seam primitive, never
// takes the cooperative baton, and only touches atomics — so it does not perturb
// the "no real concurrency" invariant.
//
// Env: SB_WATCHDOG_SECS=<n>  timeout in seconds (default 8; 0 disables).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <execinfo.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace {

std::atomic<unsigned long long> g_heartbeat{0};
std::atomic<unsigned long>      g_run_pth{0};   // pthread_t of the running fiber
std::atomic<bool>               g_started{false};
std::atomic<bool>               g_fired{false};
int                             g_timeout_secs = 8;

std::atomic<int> g_bt_done{0};   // counts threads that finished their SIGUSR1 backtrace

// itoa for a fd write (async-signal-safe; no snprintf in a signal handler).
int as_itoa(unsigned long v, char* buf) {
    char tmp[24]; int i = 0;
    if (!v) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    int n = 0; while (i) buf[n++] = tmp[--i];
    return n;
}

// SIGUSR1 handler — runs ON each signalled thread, so its backtrace is THAT thread's stuck stack.
// The monitor signals EVERY thread (not just the running fiber) so a cooperative-scheduler DEADLOCK
// is fully visible (which worker is parked waiting on what), not just the one thread that happened
// to hold the baton. Prints a tid-labelled backtrace and RETURNS (the monitor does the final exit
// once all threads have dumped). Async-signal-safe output only (write + backtrace_symbols_fd).
void bt_handler(int /*sig*/) {
    void* frames[80];
    int n = backtrace(frames, 80);
    char line[64]; int p = 0;
    const char* a = "\n--- WATCHDOG thread tid="; while (*a) line[p++] = *a++;
    p += as_itoa((unsigned long)syscall(SYS_gettid), line + p);
    const char* b = " backtrace ---\n"; while (*b) line[p++] = *b++;
    ssize_t w = write(2, line, p); (void)w;
    backtrace_symbols_fd(frames, n, 2);
    g_bt_done.fetch_add(1, std::memory_order_relaxed);
}

// SIGSEGV/SIGABRT/SIGBUS handler — prints a backtrace of the faulting stack then
// hard-exits. Async-signal-safe output only. Installed by sb_watchdog_init so any
// native-boot crash names its own site instead of dying silently under headless run.
void crash_handler(int sig) {
    void* frames[80];
    int n = backtrace(frames, 80);
    char hdr[96];
    int len = std::snprintf(hdr, sizeof(hdr),
        "\n=== CRASH: fatal signal %d — faulting-thread backtrace ===\n", sig);
    ssize_t w = write(2, hdr, len); (void)w;
    backtrace_symbols_fd(frames, n, 2);
    static const char ftr[] = "=== CRASH: exiting (139) ===\n";
    w = write(2, ftr, sizeof(ftr) - 1); (void)w;
    _exit(139);
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
        const int pid = (int)getpid();
        const int montid = (int)syscall(SYS_gettid);
        const unsigned long runtid =
            (unsigned long)0;  // (the running fiber is included in the all-thread sweep below)
        (void)runtid;
        std::fprintf(stderr,
            "\n=== WATCHDOG: process STUCK (no forward progress for %ds) ===\n"
            "Dumping ALL thread backtraces so the deadlock is visible (which thread waits on what).\n"
            "The RUNNING fiber's stack is the immediate stall; parked workers show the wait graph.\n",
            stalled);
        std::fflush(stderr);
        // Signal EVERY thread (except the monitor itself) so each prints its own stack. Iterate
        // /proc/self/task — a normal-thread context (the monitor), so opendir/readdir is fine; only
        // the per-thread handler runs in signal context. Stagger the signals so the labelled
        // backtraces don't interleave into mush.
        int signalled = 0;
        if (DIR* d = opendir("/proc/self/task")) {
            while (struct dirent* de = readdir(d)) {
                if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
                int tid = std::atoi(de->d_name);
                if (tid == montid) continue;          // don't dump the monitor's own (uninteresting) stack
                if (syscall(SYS_tgkill, pid, tid, SIGUSR1) == 0) {
                    ++signalled;
                    std::this_thread::sleep_for(std::chrono::milliseconds(60));  // let its handler finish first
                }
            }
            closedir(d);
        }
        // Brief grace for the last handler(s), then footer + hard exit.
        for (int i = 0; i < 20 && g_bt_done.load() < signalled; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::fprintf(stderr, "=== WATCHDOG: dumped %d/%d threads — exiting (134) ===\n",
                     g_bt_done.load(), signalled);
        std::fflush(stderr);
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

    // Fatal-signal backtrace handler (SEGV/ABRT/BUS/FPE) — runs on its own small
    // altstack so a stack-overflow fault can still print. Installed unconditionally
    // (independent of the watchdog timeout), so any native-boot crash names its own
    // site instead of dying silently under a headless run.
    static char s_sigstk[131072];  // fixed (glibc SIGSTKSZ is no longer a constant)
    stack_t ss; std::memset(&ss, 0, sizeof(ss));
    ss.ss_sp = s_sigstk; ss.ss_size = sizeof(s_sigstk); ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);
    struct sigaction ca;
    std::memset(&ca, 0, sizeof(ca));
    ca.sa_handler = crash_handler;
    sigemptyset(&ca.sa_mask);
    ca.sa_flags = SA_ONSTACK;
    sigaction(SIGSEGV, &ca, nullptr);
    sigaction(SIGBUS,  &ca, nullptr);
    sigaction(SIGFPE,  &ca, nullptr);
    sigaction(SIGABRT, &ca, nullptr);

    const char* e = std::getenv("SB_WATCHDOG_SECS");
    if (e && *e) {
        int v = std::atoi(e);
        if (v <= 0) return;          // SB_WATCHDOG_SECS=0 (or invalid) disables monitor
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
