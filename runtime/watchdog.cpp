#include "watchdog.h"
#include "probe_server.h"   // reuse the dispatch counters (spin vs block)

#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include <pthread.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/resource.h>

#ifdef HAVE_DOLPHIN_CORE
#  include "Core/System.h"
#  include "Core/PowerPC/PowerPC.h"
#  include "Core/CoreTiming.h"
#  include "Core/HW/SystemTimers.h"
#  include "VideoCommon/VideoEvents.h"
#endif

namespace {

std::atomic<uint64_t> g_fields{0};            // VI-field heartbeat (pet from vi_end_field_event)
std::atomic<pthread_t> g_emu_thread{};
std::atomic<bool> g_emu_recorded{false};

std::atomic<bool> g_bt_done{false};
void*  g_bt_addrs[128];                        // emu-thread backtrace captured by the SIGUSR2 handler
std::atomic<int> g_bt_n{0};

// On-demand freeze dump: `kill -QUIT <pid>` sets this; the watchdog thread does the (non-async-
// signal-safe) dump_freeze on its next tick. Lets us capture the guest PC + symbolized recomp call
// chain DURING a freeze the heartbeat can't auto-detect (e.g. a spin that keeps VI fields ticking).
std::atomic<bool> g_force_dump{false};
void wd_sigquit(int) { g_force_dump.store(true); }

std::atomic<bool> g_parked{false};            // a thread parked deliberately (see sunbright_park)

#ifdef HAVE_DOLPHIN_CORE
Common::EventHook g_field_hook;               // kept alive to stay subscribed
#endif

void wd_write(int fd, const char* s) { (void)!write(fd, s, std::strlen(s)); }

// Runs ON the emu thread when the watchdog signals it: capture its native backtrace (innermost
// frame = the stuck recomp function). Only async-signal-safe work here (backtrace() qualifies);
// the watchdog thread symbolizes the addresses afterwards.
void wd_sigusr2(int) {
    g_bt_n.store(backtrace(g_bt_addrs, 128));
    g_bt_done.store(true);
}

// Resolve the captured addresses to recomp/runtime function names with addr2line (the recomp
// func_* are static, so absent from the dynamic symtab that backtrace_symbols would use), and
// append the named frames to the dump. Runs on the watchdog thread (popen is fine here).
void symbolize_backtrace(int fd, int n) {
    char exe[256]; ssize_t el = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (el <= 0) return; exe[el] = 0;
    std::string cmd = "addr2line -f -C -e '"; cmd += exe; cmd += '\'';
    char a[32];
    for (int i = 0; i < n; i++) { std::snprintf(a, sizeof a, " %p", g_bt_addrs[i]); cmd += a; }
    cmd += " 2>/dev/null";
    if (FILE* p = popen(cmd.c_str(), "r")) {
        char buf[1024]; int frame = 0; bool name_line = true; char fn[1024] = {0};
        while (std::fgets(buf, sizeof buf, p)) {
            buf[strcspn(buf, "\n")] = 0;
            if (name_line) { std::snprintf(fn, sizeof fn, "%s", buf); name_line = false; }
            else {
                char line[1280];
                std::snprintf(line, sizeof line, "  #%-2d %s  (%s)\n", frame++, fn, buf);
                wd_write(fd, line);
                name_line = true;
            }
        }
        pclose(p);
    }
}

void dump_freeze(int timeout_sec) {
    (void)mkdir("scratch", 0755);
    (void)mkdir("scratch/watchdog", 0755);
    char path[256];
    std::time_t t = std::time(nullptr);
    std::tm tm{}; localtime_r(&t, &tm);
    std::snprintf(path, sizeof path, "scratch/watchdog/freeze_%04d%02d%02d_%02d%02d%02d.txt",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { std::fprintf(stderr, "[watchdog] FROZE but could not open %s\n", path); return; }

    char line[512];
    std::snprintf(line, sizeof line,
        "Sunbright freeze watchdog\n=========================\n"
        "No VI field presented for %d s (heartbeat stuck at %llu fields) while the core kept running.\n\n",
        timeout_sec, (unsigned long long)g_fields.load());
    wd_write(fd, line);

    // Spin vs block: sample the dispatch counters across a short window. If recomp/tail keep
    // climbing, the emu thread is spinning in a loop; if they're frozen, it's blocked/deadlocked.
    const uint64_t r0 = g_probe.call_recomp.load(), t0 = g_probe.tail.load(),
                   i0 = g_probe.interp_steps.load(), p0 = g_probe.poll_yield.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const uint64_t dr = g_probe.call_recomp.load() - r0, dt = g_probe.tail.load() - t0,
                   di = g_probe.interp_steps.load() - i0, dp = g_probe.poll_yield.load() - p0;
    std::snprintf(line, sizeof line,
        "Dispatch over 300ms:  recomp +%llu  tail +%llu  interp_steps +%llu  poll_yield +%llu\n"
        "Diagnosis: %s\n\n",
        (unsigned long long)dr, (unsigned long long)dt, (unsigned long long)di, (unsigned long long)dp,
        (dr || dt || di) ? "emu thread is SPINNING (recomp still executing — infinite loop / busy-wait)"
                         : "emu thread is BLOCKED (no recomp progress — deadlock / stuck wait)");
    wd_write(fd, line);

#ifdef HAVE_DOLPHIN_CORE
    auto& sys = Core::System::GetInstance();
    auto& ppc = sys.GetPPCState();
    std::snprintf(line, sizeof line,
        "Guest CPU (last synced — recomp runs on the host stack, so the backtrace below is authoritative):\n"
        "  pc=%08x  lr=%08x  ctr=%08x  msr=%08x  srr0=%08x  srr1=%08x\n",
        ppc.pc, ppc.spr[SPR_LR], ppc.spr[SPR_CTR], ppc.msr.Hex, ppc.spr[26], ppc.spr[27]);
    wd_write(fd, line);
    for (int i = 0; i < 32; i += 4) {
        std::snprintf(line, sizeof line, "  r%-2d=%08x  r%-2d=%08x  r%-2d=%08x  r%-2d=%08x\n",
                      i, ppc.gpr[i], i + 1, ppc.gpr[i + 1], i + 2, ppc.gpr[i + 2], i + 3, ppc.gpr[i + 3]);
        wd_write(fd, line);
    }
    std::snprintf(line, sizeof line, "  CoreTiming ticks=%llu  fakeTB=%llu\n",
                  (unsigned long long)sys.GetCoreTiming().GetTicks(),
                  (unsigned long long)sys.GetSystemTimers().GetFakeTimeBase());
    wd_write(fd, line);
#endif

    // Native backtrace of the stuck emu thread (the real locator: the recomp call chain).
    if (g_emu_recorded.load()) {
        g_bt_done.store(false);
        g_bt_n.store(0);
        pthread_kill(g_emu_thread.load(), SIGUSR2);
        for (int i = 0; i < 200 && !g_bt_done.load(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));   // up to ~2s
        if (g_bt_done.load()) {
            wd_write(fd, "\n--- emu-thread native backtrace (innermost = stuck point) ---\n");
            symbolize_backtrace(fd, g_bt_n.load());
        } else {
            wd_write(fd, "\n(emu thread did not respond to the backtrace signal — deeply blocked in the kernel)\n");
        }
    } else {
        wd_write(fd, "\n(emu thread not yet recorded — no native backtrace)\n");
    }

    close(fd);
    std::fprintf(stderr, "[watchdog] FREEZE detected — context written to %s\n", path);
    std::fflush(stderr);
}

// "Core has started executing" detector. CoreTiming ticks advance whenever guest time moves
// (Dolphin's CPU loop / JIT / interpreter). Used ONLY to arm the watchdog (so it doesn't fire
// before boot begins) — NOT as the freeze heartbeat: a guest spin-WAIT (e.g. a timed poll loop on
// mftb waiting for an event that never arrives — the post-w1stLoad audio-init stall) keeps burning
// guest cycles, so ticks keep advancing even though the game is frozen. The only signal that the
// game is actually making forward progress is a presented VI field (vi_end_field, driven by the VI
// hardware emulation — fires headless too, it is NOT gated by swapchain present).
uint64_t guest_ticks() {
#ifdef HAVE_DOLPHIN_CORE
    return Core::System::GetInstance().GetCoreTiming().GetTicks();
#else
    return 0;
#endif
}

void watchdog_loop(int timeout_sec) {
    // SUNBRIGHT_WATCHDOG_TEST=1: once the game is running, force a single dump to validate the
    // full path (file + counters + guest state + emu-thread backtrace) without a real freeze.
    const bool selftest = getenv("SUNBRIGHT_WATCHDOG_TEST") != nullptr;
    // First-frame grace: from "core started" to the first VI field a healthy boot legitimately
    // presents nothing for a while (OS/apploader/asset load). Give that window a longer budget;
    // once frames flow, hold to the tight timeout. A boot that NEVER reaches its first field within
    // the grace is itself a freeze (this is exactly the post-w1stLoad stall) and must fire.
    const int boot_grace = timeout_sec > 20 ? timeout_sec : 20;
    uint64_t last_fields = g_fields.load();
    uint64_t last_ticks  = guest_ticks();
    int stalled = 0;
    bool armed = false, first_field = false, fired = false, selftest_done = false;
    const bool dbg = getenv("SUNBRIGHT_WATCHDOG_DEBUG") != nullptr;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (g_force_dump.exchange(false)) {            // `kill -QUIT <pid>` → on-demand dump
            std::fprintf(stderr, "[watchdog] on-demand dump (SIGQUIT)\n");
            dump_freeze(timeout_sec);
        }
        const uint64_t cur_fields = g_fields.load();
        const uint64_t cur_ticks  = guest_ticks();
        if (dbg)
            std::fprintf(stderr, "[watchdog] fields=%llu ticks=%llu armed=%d first_field=%d stalled=%d\n",
                         (unsigned long long)cur_fields, (unsigned long long)cur_ticks, armed, first_field, stalled);
        if (cur_ticks != last_ticks) { last_ticks = cur_ticks; armed = true; }  // core is executing
        // A presented VI field is the ONLY real forward-progress signal (see guest_ticks comment).
        if (cur_fields != last_fields) {
            last_fields = cur_fields; stalled = 0; fired = false; first_field = true;
            if (selftest && !selftest_done) { selftest_done = true; dump_freeze(timeout_sec); }
            continue;
        }
        if (g_parked.load()) continue;                 // deliberate diagnostic park — not a freeze
        if (!armed) continue;                          // core hasn't started yet — don't fire pre-boot
        const int limit = first_field ? timeout_sec : boot_grace;
        if (++stalled >= limit && !fired) {            // one dump per freeze episode
            dump_freeze(timeout_sec);
            fired = true;
            // A freeze is ALWAYS fatal: report the stack trace (above) then KILL the process — a hung
            // run must never sit forever, and headless/CI runs must surface the freeze as a non-zero
            // exit. Not env-gated: a freeze is a bug, we always stop on it.
            std::fprintf(stderr, "[watchdog] FROZE — killing process (exit 137).\n");
            std::fflush(stderr);
            struct rlimit no_core{0, 0};               // skip the multi-GB core dump (shutdown hang)
            setrlimit(RLIMIT_CORE, &no_core);
            _exit(137);
        }
    }
}

}  // namespace

unsigned long long watchdog_vi_fields() { return g_fields.load(); }

[[noreturn]] void sunbright_park(const char* reason) {
    g_parked.store(true);
    std::fprintf(stderr, "[sunbright] PARKED (%s) — process held CPU-idle for REPL inspection "
                         "(http://127.0.0.1:17654 if SUNBRIGHT_PROBE=1); kill when done.\n",
                 reason ? reason : "diagnostic");
    std::fflush(stderr);
    struct rlimit no_core{0, 0};               // a later manual kill must not dump the multi-GB core
    setrlimit(RLIMIT_CORE, &no_core);
    for (;;) sleep(3600);
}

void watchdog_register_emu_thread() {
    if (g_emu_recorded.load(std::memory_order_relaxed)) return;
    g_emu_thread.store(pthread_self());
    g_emu_recorded.store(true);
}

void watchdog_init() {
    static bool started = false;
    if (started) return;
    const char* dis = getenv("SUNBRIGHT_WATCHDOG");
    if (dis && atoi(dis) == 0) return;                 // explicitly disabled
    started = true;

    int timeout_sec = 5;
    if (const char* s = getenv("SUNBRIGHT_WATCHDOG_SEC")) { int v = atoi(s); if (v > 0) timeout_sec = v; }

    // The dispatch counters drive the spin-vs-block diagnosis; make them live even without the probe.
    g_probe_enabled = true;

    struct sigaction sa{};
    sa.sa_handler = wd_sigusr2;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &sa, nullptr);

    struct sigaction sq{};                             // SIGQUIT → on-demand freeze dump
    sq.sa_handler = wd_sigquit;
    sigemptyset(&sq.sa_mask);
    sq.sa_flags = SA_RESTART;
    sigaction(SIGQUIT, &sq, nullptr);

#ifdef HAVE_DOLPHIN_CORE
    g_field_hook = GetVideoEvents().vi_end_field_event.Register(
        [] { g_fields.fetch_add(1, std::memory_order_relaxed); });
#endif

    std::thread(watchdog_loop, timeout_sec).detach();
    std::fprintf(stderr, "[watchdog] armed (fires after %ds with no VI field while the core runs; "
                 "SUNBRIGHT_WATCHDOG=0 to disable)\n", timeout_sec);
}
