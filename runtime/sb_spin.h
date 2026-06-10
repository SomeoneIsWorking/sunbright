// Busy-spin accounting — the watchdog spin detector's data source.
//
// Every POLLING wait loop in the runtime (a loop that repeatedly checks a condition, possibly
// with a short sleep, instead of blocking on an event) wraps its iteration in SB_SPIN_GUARD.
// The guard accumulates wall time + iteration count per site; the watchdog samples the totals
// once per second and declares a busy-spin when wait-loop time dominates wall time while the
// game presents (almost) no frames — then dumps the per-site table + backtrace and kills.
//
// Rule: only instrument POLLING iterations. A true blocking wait (condvar, sleep_until pacing)
// must NOT be wrapped — time spent properly blocked is not a spin.
#pragma once

#include <atomic>
#include <chrono>

struct SbSpinSite {
    const char* name;
    std::atomic<unsigned long long> us{0};      // wall microseconds inside the loop (outermost only)
    std::atomic<unsigned long long> iters{0};
    SbSpinSite* next = nullptr;
    explicit SbSpinSite(const char* n);
};

SbSpinSite* sb_spin_site_list();                 // singly-linked registry (stable once registered)
unsigned long long sb_spin_total_us();

class SbSpinScope {
    static thread_local int t_depth;             // nested guards (poll_yield inside bp loop) count once
    SbSpinSite& s_;
    std::chrono::steady_clock::time_point t0_;
    bool outer_;
public:
    explicit SbSpinScope(SbSpinSite& s) : s_(s) {
        outer_ = (t_depth++ == 0);
        if (outer_) t0_ = std::chrono::steady_clock::now();
        s_.iters.fetch_add(1, std::memory_order_relaxed);
    }
    ~SbSpinScope() {
        --t_depth;
        if (outer_)
            s_.us.fetch_add((unsigned long long)std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0_).count(),
                            std::memory_order_relaxed);
    }
};

// One per loop body: `for (...) { SB_SPIN_GUARD("vi.gpu_backpressure"); ... }`
#define SB_SPIN_GUARD(name_str)                          \
    static SbSpinSite _sb_spin_site(name_str);           \
    SbSpinScope _sb_spin_scope(_sb_spin_site)
