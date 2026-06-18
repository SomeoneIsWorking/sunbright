// Pure-JIT pre-hook probe (SUNBRIGHT_PUREJIT=1).
//
// Registers OBSERVE pre-hooks on the two ngx capture seams. ⚠ FINDING (2026-06-18, session 10): a
// pre-hook (observe + return-false-so-Dolphin-JITs-the-original) fires only ONCE per block — the
// fork's JitTrampoline runs on the dispatcher's CACHE-MISS/COMPILE path, so once Dolphin compiles a
// passthrough block at the address, every later dispatch runs the cached block WITHOUT re-entering
// our hook. This probe printing "J3DShape::draw=1" after a whole scene of rendering IS that bug, not
// success. Pre-hooks are therefore useless for PER-CALL engine capture; the ngx capture seams must
// instead be purejit-safe RETURN-TRUE overrides (full native replacement — see the journal). Kept as
// a diagnostic that demonstrates the once-per-compile limitation; do NOT build capture on it.

#include "../overrides.h"
#include <cstdio>
#include <cstdlib>

namespace {

void observe_seam(CPUState& cpu) {
    static unsigned long n_shape = 0, n_screen = 0, total = 0, last = 0;
    if (cpu.pc == 0x802e0390u) ++n_shape;        // J3DShape::draw
    else if (cpu.pc == 0x802d01c8u) ++n_screen;  // J2DScreen::drawSelf
    if (total == 0 || (++total - last) >= 2000) {
        if (total == 0) ++total;
        last = total;
        fprintf(stderr, "[purejit] pre-hook seams hit: J3DShape::draw=%lu J2DScreen::drawSelf=%lu\n",
                n_shape, n_screen);
    }
}

const bool registered = [] {
    if (!getenv("SUNBRIGHT_PUREJIT")) return false;
    register_prehook(0x802e0390u, &observe_seam);   // J3DShape::draw
    register_prehook(0x802d01c8u, &observe_seam);   // J2DScreen::drawSelf
    return true;
}();

}  // namespace
