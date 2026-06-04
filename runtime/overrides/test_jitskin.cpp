// Diagnostic: route an address range to Dolphin's JIT (SUNBRIGHT_JIT_LO/HI, hex) to bisect a
// recomp miscompilation. NOTE: force-JIT of heavily-used game-logic/matrix functions can hang the
// game at the recomp↔JIT boundary (FPR/matrix state sync) — keep ranges small and verify the game
// still progresses, or this misleads.
#include "../overrides.h"
#include <cstdlib>
#include <cstdio>
static const bool s_reg = [] {
    if (const char* lo = getenv("SUNBRIGHT_JIT_LO")) {
        u32 l = (u32)strtoul(lo, nullptr, 16), h = (u32)strtoul(getenv("SUNBRIGHT_JIT_HI"), nullptr, 16);
        force_jit_range(l, h);
        std::fprintf(stderr, "[test] forced JIT range 0x%08x-0x%08x\n", l, h);
    }
    return true;
}();
