// Pure-JIT pre-hook validation probe (SUNBRIGHT_PUREJIT=1).
//
// Registers OBSERVE pre-hooks on the two ngx capture seams to prove the pre-hook table + trampoline
// wiring work end-to-end: under pure Dolphin JIT (no recomp / native_os / exec-model overrides), the
// trampoline runs these pre-hooks and then lets Dolphin JIT the original block. If boot progresses
// and the liveness line prints, the no-recomp engine-seam mechanism is real (Phase B step 1). This
// file is throwaway scaffolding for the real engine pre-hooks (ngx capture, fastboot) — replace it.

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
