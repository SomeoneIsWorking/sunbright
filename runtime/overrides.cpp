#include "overrides.h"
#undef register_override                 // we DEFINE register_override_impl here; keep the name clean
#include <unordered_map>
#include <vector>
#include <utility>
#include <string>
#include <cstdlib>
#include <cstring>
#include <cstdio>

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

// ── NO_RECOMP override-group bisection ───────────────────────────────────────
// Returns the basename of a __FILE__ path (after the last '/').
static const char* base_name(const char* path) {
    const char* slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// True if `group` (an override's source file) matches any comma-separated substring in `list`.
static bool group_matches(const char* group, const char* list) {
    const char* bn = base_name(group);
    std::string l(list);
    size_t start = 0;
    while (start <= l.size()) {
        size_t comma = l.find(',', start);
        std::string tok = l.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!tok.empty() && (std::strstr(bn, tok.c_str()) || std::strstr(group, tok.c_str())))
            return true;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}

// Decide whether an override in file `group` should be skipped (routed to Dolphin JIT) for the
// NO_RECOMP boot-hang bisection. Only active when SUNBRIGHT_NO_RECOMP is set.
static bool override_group_skipped(const char* group) {
    static const bool no_recomp = std::getenv("SUNBRIGHT_NO_RECOMP") != nullptr;
    if (!no_recomp) return false;
    static const char* only = std::getenv("SUNBRIGHT_NORECOMP_ONLY");
    static const char* skip = std::getenv("SUNBRIGHT_NORECOMP_SKIP");
    if (only && *only && !group_matches(group, only)) return true;   // whitelist: keep only matches
    if (skip && *skip &&  group_matches(group, skip)) return true;   // blacklist: drop matches
    return false;
}

void register_override_impl(u32 addr, RecompFunc fn, const char* group) {
    if (override_group_skipped(group)) {
        static const bool log = std::getenv("SUNBRIGHT_NORECOMP_DBG") != nullptr;
        if (log) fprintf(stderr, "[norecomp-skip] %08x (%s)\n", addr, base_name(group));
        return;                                                      // not registered → JIT
    }
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

// ── Pre-hook table (observe-then-Dolphin-JITs-original; no-recomp engine seam) ─
static std::unordered_map<u32, PreHookFn>& prehook_table() {
    static std::unordered_map<u32, PreHookFn> t;
    return t;
}
static u32 g_ph_lo = 0xFFFFFFFFu, g_ph_hi = 0;   // [lo,hi) cheap reject (trampoline hot path)

void register_prehook(u32 addr, PreHookFn fn) {
    prehook_table()[addr] = fn;
    if (addr < g_ph_lo) g_ph_lo = addr;
    if (addr + 4 > g_ph_hi) g_ph_hi = addr + 4;
}

PreHookFn prehook_lookup(u32 addr) {
    if (addr < g_ph_lo || addr >= g_ph_hi) return nullptr;
    auto& t = prehook_table();
    auto it = t.find(addr);
    return it == t.end() ? nullptr : it->second;
}

bool prehooks_registered() { return !prehook_table().empty(); }

bool sunbright_purejit_mode() {
    static const bool on = std::getenv("SUNBRIGHT_PUREJIT") != nullptr;
    return on;
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
