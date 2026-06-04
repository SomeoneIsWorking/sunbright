// Diagnostic: route an arbitrary set of guest addresses back to Dolphin's JIT at runtime,
// without regenerating. Set SUNBRIGHT_FORCE_JIT to a comma/space-separated list of hex
// addresses (0x-prefixed or bare), e.g. SUNBRIGHT_FORCE_JIT=8031d83c,8001fa88. A token of the
// form LO-HI is a half-open RANGE (force_jit_range), e.g. 80310000-80330000 — for binary-searching
// which region holds the misbehaving recompiled function. Use to bisect which recompiled function
// hangs/miscompiles by selectively reverting it to the JIT — a fast A/B without a recompile cycle.
// Keepable diagnostic.
#include "../overrides.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>

static const bool forcejit_env_reg = [] {
    const char* s = getenv("SUNBRIGHT_FORCE_JIT");
    if (!s) return true;
    char buf[4096]; std::strncpy(buf, s, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    for (char* tok = std::strtok(buf, ", \t"); tok; tok = std::strtok(nullptr, ", \t")) {
        char* dash = std::strchr(tok, '-');
        if (dash) {                                    // LO-HI range
            *dash = 0;
            u32 lo = (u32)std::strtoul(tok, nullptr, 16);
            u32 hi = (u32)std::strtoul(dash + 1, nullptr, 16);
            if (lo >= 0x80000000u && hi > lo && hi <= 0x81800000u) {
                force_jit_range(lo, hi);
                std::fprintf(stderr, "[forcejit_env] routing 0x%08x-0x%08x to Dolphin JIT\n", lo, hi);
            }
            continue;
        }
        u32 addr = (u32)std::strtoul(tok, nullptr, 16);
        if (addr >= 0x80000000u && addr < 0x81800000u) {
            force_jit(addr);
            std::fprintf(stderr, "[forcejit_env] routing 0x%08x to Dolphin JIT\n", addr);
        }
    }
    return true;
}();
