// Diagnostic: route an arbitrary set of guest addresses back to Dolphin's JIT at runtime,
// without regenerating. Set SUNBRIGHT_FORCE_JIT to a comma/space-separated list of hex
// addresses (0x-prefixed or bare), e.g. SUNBRIGHT_FORCE_JIT=8031d83c,8001fa88. Use to
// bisect which recompiled function misbehaves (hangs/miscompiles) by selectively reverting
// it to the JIT — a fast A/B without a 2-minute recompile cycle. Keepable diagnostic.
#include "../overrides.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>

static const bool forcejit_env_reg = [] {
    const char* s = getenv("SUNBRIGHT_FORCE_JIT");
    if (!s) return true;
    char buf[4096]; std::strncpy(buf, s, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    for (char* tok = std::strtok(buf, ", \t"); tok; tok = std::strtok(nullptr, ", \t")) {
        u32 addr = (u32)std::strtoul(tok, nullptr, 16);
        if (addr >= 0x80000000u && addr < 0x81800000u) {
            force_jit(addr);
            std::fprintf(stderr, "[forcejit_env] routing 0x%08x to Dolphin JIT\n", addr);
        }
    }
    return true;
}();
