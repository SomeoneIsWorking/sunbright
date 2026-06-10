#include "sb_spin.h"

static std::atomic<SbSpinSite*> g_head{nullptr};

SbSpinSite::SbSpinSite(const char* n) : name(n) {
    next = g_head.load(std::memory_order_relaxed);
    while (!g_head.compare_exchange_weak(next, this, std::memory_order_release,
                                         std::memory_order_relaxed)) {}
}

SbSpinSite* sb_spin_site_list() { return g_head.load(std::memory_order_acquire); }

unsigned long long sb_spin_total_us() {
    unsigned long long t = 0;
    for (SbSpinSite* s = sb_spin_site_list(); s; s = s->next)
        t += s->us.load(std::memory_order_relaxed);
    return t;
}

thread_local int SbSpinScope::t_depth = 0;
