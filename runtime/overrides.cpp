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

void register_override(u32 addr, RecompFunc fn) {
    override_table()[addr] = fn;
}

RecompFunc override_lookup(u32 addr) {
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
