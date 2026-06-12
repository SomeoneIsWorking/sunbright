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

// PSMTXIdentity(m) @ 0x80349990
SUNBRIGHT_OVERRIDE(ov_PSMTXIdentity, 0x80349990u) {
    const uint32_t m = cpu.gpr[3];
    static const float ident[12] = { 1,0,0,0, 0,1,0,0, 0,0,1,0 };
    for (int i = 0; i < 12; i++) sb_wf32(m + i * 4, ident[i]);
}

// PSMTXConcat(a, b, dst) @ 0x803499f0 — 3x4 * 3x4. The paired-single original
// accumulates with FUSED madds (ps_muls0 then ps_madds0 chain): one rounding per
// madd — host fmaf reproduces that exactly. Reads complete before writes (the SDK
// supports dst aliasing a or b; the PS version stages rows in registers).
static void mtx_concat(const float a[12], const float b[12], float d[12]) {
    for (int i = 0; i < 3; i++) {
        const float a0 = a[i*4], a1 = a[i*4+1], a2 = a[i*4+2], a3 = a[i*4+3];
        for (int j = 0; j < 4; j++) {
            float v = fmaf(a2, b[8+j], fmaf(a1, b[4+j], a0 * b[j]));
            if (j == 3) v += a3;
            d[i*4+j] = v;
        }
    }
}
SUNBRIGHT_OVERRIDE(ov_PSMTXConcat, 0x803499f0u) {
    const uint32_t pa = cpu.gpr[3], pb = cpu.gpr[4], pd = cpu.gpr[5];
    float a[12], b[12], d[12];
    for (int i = 0; i < 12; i++) { a[i] = sb_rf32(pa + i*4); b[i] = sb_rf32(pb + i*4); }
    mtx_concat(a, b, d);
    if (math_shadow()) {
        if (RecompFunc orig = recomp_raw(0x803499f0u)) {
            CPUState shadow = cpu;
            orig(shadow);                              // guest writes dst
            static unsigned long bad = 0;
            for (int i = 0; i < 12; i++) {
                const float g = sb_rf32(pd + i*4);
                if (g != d[i] && !(g != g && d[i] != d[i]) && bad < 8) {
                    bad++;
                    fprintf(stderr, "[mathshadow] PSMTXConcat MISMATCH [%d] native=%a guest=%a\n",
                            i, d[i], g);
                }
            }
        }
    }
    for (int i = 0; i < 12; i++) sb_wf32(pd + i*4, d[i]);
}

// PSMTXMultVec(m, src, dst) @ 0x8034a2d0 — v' = M*v + t, fused-madd chain.
SUNBRIGHT_OVERRIDE(ov_PSMTXMultVec, 0x8034a2d0u) {
    const uint32_t pm = cpu.gpr[3], ps = cpu.gpr[4], pd = cpu.gpr[5];
    float m[12], v[3], o[3];
    for (int i = 0; i < 12; i++) m[i] = sb_rf32(pm + i*4);
    for (int i = 0; i < 3; i++)  v[i] = sb_rf32(ps + i*4);
    for (int i = 0; i < 3; i++)
        o[i] = fmaf(m[i*4+2], v[2], fmaf(m[i*4+1], v[1], m[i*4] * v[0])) + m[i*4+3];
    for (int i = 0; i < 3; i++) sb_wf32(pd + i*4, o[i]);
}

// __cvt_fp2unsigned @ 0x8033829c — f64→u32 with saturation (compiler helper for
// (u32)float casts): NaN/≤0 → 0, ≥2^32 → 0xFFFFFFFF, else truncate.
SUNBRIGHT_OVERRIDE(ov_cvt_fp2unsigned, 0x8033829cu) {
    const double x = cpu.fpr[1].ps0;
    uint32_t r;
    if (!(x > 0.0)) r = 0;
    else if (x >= 4294967296.0) r = 0xFFFFFFFFu;
    else r = (uint32_t)x;
    if (math_shadow()) {
        if (RecompFunc orig = recomp_raw(0x8033829cu)) {
            CPUState shadow = cpu;
            orig(shadow);
            static unsigned long bad = 0;
            if (shadow.gpr[3] != r && bad++ < 8)
                fprintf(stderr, "[mathshadow] cvt_fp2unsigned MISMATCH native=%u guest=%u x=%g\n",
                        r, shadow.gpr[3], x);
        }
    }
    cpu.gpr[3] = r;
}
