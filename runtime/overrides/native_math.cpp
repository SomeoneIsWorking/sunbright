// Native ports of the hottest pure-math engine functions (call census tier 1,
// docs/decomp/runtime_coverage.md). All are side-effect-free leaf functions whose
// float semantics are exactly reproducible on the host (single-precision IEEE adds/
// muls, round-to-nearest — identical to PPC fadds/fmuls; fabs is a sign-bit clear).
// Binary-verified against --disasm; addresses from reference/sms_gmse01_funcs.txt.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../cpu_state.h"
#include "../overrides.h"
#include "../intrinsics.h"

// JGeometry checkDistance(const TVec3f& a, float aH, float aR, const TVec3f& b,
//                         float bR, float bH) @ 0x8021c2f0 — 37.1M calls/run, the
// hottest function in the game (actor/map proximity gate). Disasm semantics:
//   if (a.y > b.y + bH) return 0;          // vertical interval overlap
//   if (b.y > a.y + aH) return 0;
//   rs = aR + bR;                          // horizontal circle test (XZ plane)
//   return (a.x-b.x)^2 + (a.z-b.z)^2 <= rs^2;
// SUNBRIGHT_MATH_SHADOW=1: every native math port ALSO runs the original recompiled
// body on the same live inputs and compares — the verification harness for pure-
// function ports (real gameplay data, not synthetic samples). Mismatches log once
// per function with full operands.
static bool math_shadow() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("SUNBRIGHT_MATH_SHADOW"); v = (e && *e && *e != '0') ? 1 : 0; }
    return v;
}
extern RecompFunc recomp_raw(uint32_t address);

SUNBRIGHT_OVERRIDE(ov_jg_checkDistance, 0x8021c2f0u) {
    const uint32_t a = cpu.gpr[3], b = cpu.gpr[4];
    // float arg roles per the BINARY (shadow harness caught the swapped first guess):
    // f1/f3 = radii (summed for the XZ circle test), f2/f4 = vertical extents
    const float aR = (float)cpu.fpr[1].ps0, aH = (float)cpu.fpr[2].ps0;
    const float bR = (float)cpu.fpr[3].ps0, bH = (float)cpu.fpr[4].ps0;
    const float ay = sb_rf32(a + 4), by = sb_rf32(b + 4);
    uint32_t res = 0;
    if (!(ay > by + bH) && !(by > ay + aH)) {
        const float rs = aR + bR;
        const float dx = sb_rf32(a) - sb_rf32(b);
        const float dz = sb_rf32(a + 8) - sb_rf32(b + 8);
        // STRICT less-than: the binary's cror folds LT into EQ as (rs² <= dist²) and
        // returns 0 on that — equality is OUT of range (shadow harness caught <=:
        // grid-placed objects exactly radius-sum apart flipped the result)
        res = (dx * dx + dz * dz < rs * rs) ? 1u : 0u;
    }
    if (math_shadow()) {
        if (RecompFunc orig = recomp_raw(0x8021c2f0u)) {
            CPUState shadow = cpu;
            orig(shadow);
            static unsigned long n = 0, bad = 0;
            n++;
            if (shadow.gpr[3] != res && bad++ < 8)
                fprintf(stderr, "[mathshadow] checkDistance MISMATCH native=%u guest=%u "
                                "(a=%08x b=%08x aH=%g aR=%g bR=%g bH=%g) n=%lu\n",
                        res, shadow.gpr[3], a, b, aH, aR, bR, bH, n);
        }
    }
    cpu.gpr[3] = res;
}

// fabsf @ 0x8033c224 — 13.3M calls/run. Sign-bit clear; bit-exact on host.
SUNBRIGHT_OVERRIDE(ov_fabsf, 0x8033c224u) {
    cpu.fpr[1].ps0 = fabs(cpu.fpr[1].ps0);
}

// PSMTXCopy(src, dst) @ 0x803499bc — 3.3M calls/run. 12-float (3x4) copy, done with
// paired-single loads on hardware; a straight 48-byte guest-memory copy here.
SUNBRIGHT_OVERRIDE(ov_PSMTXCopy, 0x803499bcu) {
    const uint32_t src = cpu.gpr[3], dst = cpu.gpr[4];
    for (int i = 0; i < 12; i++) sb_w32(dst + i * 4, sb_r32(src + i * 4));
}
