#include "overrides.h"
#include <unordered_map>
#include <vector>
#include <utility>

// Function-local statics so registration from other TUs' static initializers is
// order-safe (the maps are constructed on first use, before any lookup).

static std::unordered_map<u32, RecompFunc>& override_table() {
    static std::unordered_map<u32, RecompFunc> t;
    return t;
}

static std::vector<std::pair<u32, u32>>& jit_ranges() {
    static std::vector<std::pair<u32, u32>> v;
    return v;
}

// [lo, hi) bounding range of all override addresses — a cheap reject so the hashmap find runs only
// for addresses that could possibly be an override. override_lookup is on the hot call_ppc path
// (every bl/blr), so the common "not an override" case must be a couple of compares, not a hash.
static u32 g_ov_lo = 0xFFFFFFFFu, g_ov_hi = 0;

void register_override(u32 addr, RecompFunc fn) {
    override_table()[addr] = fn;
    if (addr < g_ov_lo) g_ov_lo = addr;
    if (addr + 4 > g_ov_hi) g_ov_hi = addr + 4;
}

RecompFunc override_lookup(u32 addr) {
    if (addr < g_ov_lo || addr >= g_ov_hi) return nullptr;   // cheap reject (hot path)
    auto& t = override_table();
    auto it = t.find(addr);
    return it == t.end() ? nullptr : it->second;
}

void force_jit(u32 addr) { jit_ranges().emplace_back(addr, addr + 4); }

void force_jit_range(u32 lo, u32 hi) { jit_ranges().emplace_back(lo, hi); }

bool is_jit_forced(u32 addr) {
    for (const auto& [lo, hi] : jit_ranges())
        if (addr >= lo && addr < hi) return true;
    return false;
}

// Lets the hot dispatch path skip these lookups entirely when nothing is
// registered (the common case).
bool overrides_registered()  { return !override_table().empty(); }
bool jit_forced_registered() { return !jit_ranges().empty(); }
