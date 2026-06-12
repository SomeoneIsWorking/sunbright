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

// PSMTXMultVec(m, src, dst) @ 0x8034a2d0 — v' = M*v + t. Disasm-exact rounding
// (decoded 2026-06-12, see docs/decomp/msl_trig.md PS-decoder note): per row the
// binary does ps_mul (m0*x, m1*y) → ps_madd lane0 = fmaf(m2,z,m0*x), lane1 =
// m3 + (m1*y) [m3*1.0 exact, so the fused add == plain single add] → ps_sum0
// adds the two lanes. NOT one long fmaf chain (the previous port fused
// m1*y into the chain — one rounding short on lane1's mul).
SUNBRIGHT_OVERRIDE(ov_PSMTXMultVec, 0x8034a2d0u) {
    const uint32_t pm = cpu.gpr[3], ps = cpu.gpr[4], pd = cpu.gpr[5];
    float m[12], v[3], o[3];
    for (int i = 0; i < 12; i++) m[i] = sb_rf32(pm + i*4);
    for (int i = 0; i < 3; i++)  v[i] = sb_rf32(ps + i*4);
    for (int i = 0; i < 3; i++) {
        const float t0 = fmaf(m[i*4+2], v[2], m[i*4] * v[0]);   // ps_madd lane0
        const float t1 = m[i*4+3] + m[i*4+1] * v[1];            // ps_madd lane1 (×1.0)
        o[i] = t0 + t1;                                         // ps_sum0
    }
    if (math_shadow()) {
        if (RecompFunc orig = recomp_raw(0x8034a2d0u)) {
            CPUState shadow = cpu;
            orig(shadow);
            static unsigned long bad = 0;
            for (int i = 0; i < 3; i++) {
                const float g = sb_rf32(pd + i*4);
                if (g != o[i] && !(g != g && o[i] != o[i]) && bad < 8) {
                    bad++;
                    fprintf(stderr, "[mathshadow] PSMTXMultVec MISMATCH [%d] native=%a guest=%a\n",
                            i, o[i], g);
                }
            }
        }
    }
    for (int i = 0; i < 3; i++) sb_wf32(pd + i*4, o[i]);
}

// ---------------------------------------------------------------------------
// MSL table-based trig: sinf @ 0x8033c7e4, cosf @ 0x8033c650 — 24.6M calls/run
// combined. Full decode in docs/decomp/msl_trig.md (source reference:
// reference/sms/src/PowerPC_EABI_Support/.../Single_precision/trigf.c).
// Constants/tables are read FROM GUEST RAM at the binary's own addresses, so
// the port can't drift from the data the game ships:
//   SDA2 (r2)+0xa88  __two_over_pi   0.63661975f
//   SDA2 (r2)+0xa8c  0.5f
//   SDA2 (r2)+0xa90  __SQRT_FLT_EPSILON__ 3.4526698e-4f
//   0x803e67c8       __four_over_pi_m1[4]  (RAM copy, filled by static init
//                    @ 0x8033c988 from rodata 0x803aade8)
//   0x803ab2c4       __sincos_on_quadrant[8]   (sin,cos pairs per quadrant)
//   0x803ab2e4       __sincos_poly[10]         (interleaved cos/sin coeffs)
// All madds in the binary are FUSED (fmadds/fnmadds/fnmsubs) → host fmaf.
// ---------------------------------------------------------------------------
static inline int32_t emu_fctiwz(double v) {
    // PPC fctiwz: truncate toward zero, saturate; NaN → INT32_MIN.
    if (!(v > -2147483649.0)) return INT32_MIN;       // NaN or ≤ -2^31-1 region
    if (v >= 2147483648.0)    return INT32_MAX;
    return (int32_t)v;                                 // C cast truncates ✓
}

static float msl_sincos(CPUState& cpu, bool is_cos) {
    const float x = (float)cpu.fpr[1].ps0;
    const uint32_t r2 = cpu.gpr[2];
    const float two_over_pi = sb_rf32(r2 + 0xa88u);
    const float half        = sb_rf32(r2 + 0xa8cu);
    const float eps         = sb_rf32(r2 + 0xa90u);

    const float z = two_over_pi * x;                   // fmuls
    uint32_t xb; memcpy(&xb, &x, 4);                   // rlwinm. sign-bit test
    const float zr = (xb & 0x80000000u) ? (z - half) : (z + half);  // fsubs/fadds
    const int32_t n = emu_fctiwz((double)zr);          // fctiwz

    // frac = x - (float)(n*2), then 4 fused madds with __four_over_pi_m1[k]*x.
    // n*2 via rlwinm n<<1 (bit31 of the rotate masked off) = int32 (n<<1).
    const float two_n = (float)(int32_t)((uint32_t)n << 1);
    float frac = x - two_n;                            // fsubs
    for (int k = 0; k < 4; k++)
        frac = fmaf(sb_rf32(0x803e67c8u + 4u*k), x, frac);   // fmadds chain

    const uint32_t qoff = ((uint32_t)n & 3u) * 8u;     // n&=3; quad pair index
    const uint32_t quad = 0x803ab2c4u, poly = 0x803ab2e4u;
    const float q0 = sb_rf32(quad + qoff), q1 = sb_rf32(quad + qoff + 4);
    float P[10];
    for (int k = 0; k < 10; k++) P[k] = sb_rf32(poly + 4u*k);

    if (fabsf(frac) < eps) {                           // fcmpo vs eps: small-frac path
        if (!is_cos) return fmaf(P[9], q1 * frac, q0); // fmuls + fmadds
        return -fmaf(frac, q0, -q1);                   // fnmsubs = -(frac*q0 - q1)
    }
    const float xsq = frac * frac;                     // fmuls
    // Horner chain, all fused: acc = fmaf(P[k],xsq,P[k+2]); then 3× fmaf(xsq,acc,P[k+2i])
    auto chain = [&](int k) {
        float a = fmaf(P[k], xsq, P[k + 2]);
        a = fmaf(xsq, a, P[k + 4]);
        a = fmaf(xsq, a, P[k + 6]);
        return a;                                      // last term applied per-path
    };
    if (((uint32_t)n & 1u) == 0) {
        // even quadrant
        if (!is_cos) return (frac * fmaf(xsq, chain(1), P[9])) * q1; // sin poly × frac × quad[2n+1]
        return fmaf(xsq, chain(0), P[8]) * q1;                       // cos poly × quad[2n+1]
    }
    // odd quadrant
    if (!is_cos) return fmaf(xsq, chain(0), P[8]) * q0;              // cos poly × quad[2n]
    return (frac * -fmaf(xsq, chain(1), P[9])) * q0;                 // fnmadds, × frac × quad[2n]
}

static void msl_trig_override(CPUState& cpu, uint32_t addr, bool is_cos, const char* name) {
    const float r = msl_sincos(cpu, is_cos);
    if (math_shadow()) {
        if (RecompFunc orig = recomp_raw(addr)) {
            CPUState shadow = cpu;
            orig(shadow);
            const float g = (float)shadow.fpr[1].ps0;
            static unsigned long bad = 0;
            uint32_t rb, gb; memcpy(&rb, &r, 4); memcpy(&gb, &g, 4);
            if (rb != gb && !(r != r && g != g) && bad++ < 8)
                fprintf(stderr, "[mathshadow] %s MISMATCH native=%a guest=%a x=%a\n",
                        name, r, g, (float)cpu.fpr[1].ps0);
        }
    }
    cpu.fpr[1].ps0 = r;
}

SUNBRIGHT_OVERRIDE(ov_sinf, 0x8033c7e4u) { msl_trig_override(cpu, 0x8033c7e4u, false, "sinf"); }
SUNBRIGHT_OVERRIDE(ov_cosf, 0x8033c650u) { msl_trig_override(cpu, 0x8033c650u, true,  "cosf"); }

// PSMTXMultVecSR(m, src, dst) @ 0x8034a3b0 — rotation-only (no translate column).
// Disasm: per row i: ps_mul (mi0*x, mi1*y) → ps_sum0 (single add) →
// ps_madd out = fmaf(mi2, z, sum). mi3 only ever lands in the discarded ps1 lane.
SUNBRIGHT_OVERRIDE(ov_PSMTXMultVecSR, 0x8034a3b0u) {
    const uint32_t pm = cpu.gpr[3], ps = cpu.gpr[4], pd = cpu.gpr[5];
    float m[12], v[3], o[3];
    for (int i = 0; i < 12; i++) m[i] = sb_rf32(pm + i*4);
    for (int i = 0; i < 3; i++)  v[i] = sb_rf32(ps + i*4);
    for (int i = 0; i < 3; i++) {
        const float s = m[i*4] * v[0] + m[i*4+1] * v[1];   // ps_mul + ps_sum0
        o[i] = fmaf(m[i*4+2], v[2], s);                    // ps_madd
    }
    if (math_shadow()) {
        if (RecompFunc orig = recomp_raw(0x8034a3b0u)) {
            CPUState shadow = cpu;
            orig(shadow);
            static unsigned long bad = 0;
            for (int i = 0; i < 3; i++) {
                const float g = sb_rf32(pd + i*4);
                if (g != o[i] && !(g != g && o[i] != o[i]) && bad < 8) {
                    bad++;
                    fprintf(stderr, "[mathshadow] PSMTXMultVecSR MISMATCH [%d] native=%a guest=%a\n",
                            i, o[i], g);
                }
            }
        }
    }
    for (int i = 0; i < 3; i++) sb_wf32(pd + i*4, o[i]);
}

// PSMTXMultVecArray(m, srcBase, dstBase, count) @ 0x8034a324 — full MultVec
// (translate included) over count vectors, stride 12. Disasm rounding differs
// from the single-vector MultVec: rows 0/1 are one fused madds chain seeded with
// the translate term (ps_madds0/ps_madds1/ps_madds0), row 2 is the
// mul+madd+sum0 shape. Hardware note: the binary does mtctr(count-1)+bdnz with
// a peeled iteration → count elements processed; count<1 would underflow CTR on
// hardware (SDK requires count≥1; we loop count times).
SUNBRIGHT_OVERRIDE(ov_PSMTXMultVecArray, 0x8034a324u) {
    const uint32_t pm = cpu.gpr[3], ps = cpu.gpr[4], pd = cpu.gpr[5];
    const uint32_t count = cpu.gpr[6];
    float m[12];
    for (int i = 0; i < 12; i++) m[i] = sb_rf32(pm + i*4);
    for (uint32_t e = 0; e < count; e++) {
        const float x = sb_rf32(ps + e*12u), y = sb_rf32(ps + e*12u + 4u),
                    z = sb_rf32(ps + e*12u + 8u);
        // rows 0/1: f8 = madds0(row.x, x, row.w); madds1(row.y, y, ·); madds0(row.z, z, ·)
        const float ox = fmaf(m[2],  z, fmaf(m[1],  y, fmaf(m[0], x, m[3])));
        const float oy = fmaf(m[6],  z, fmaf(m[5],  y, fmaf(m[4], x, m[7])));
        // row 2: ps_mul (m20x, m21y); ps_madd (fmaf(m22,z,·), m23 + ·); ps_sum0
        const float t0 = fmaf(m[10], z, m[8] * x);
        const float t1 = m[11] + m[9] * y;
        const float oz = t0 + t1;
        sb_wf32(pd + e*12u,      ox);
        sb_wf32(pd + e*12u + 4u, oy);
        sb_wf32(pd + e*12u + 8u, oz);
    }
    if (math_shadow()) {
        if (RecompFunc orig = recomp_raw(0x8034a324u)) {
            // Native already wrote dst; stash it, rerun guest, compare, restore.
            CPUState shadow = cpu;
            const uint32_t nf = count * 3u;
            float* nat = new float[nf];
            for (uint32_t i = 0; i < nf; i++) nat[i] = sb_rf32(pd + i*4u);
            orig(shadow);
            static unsigned long bad = 0;
            for (uint32_t i = 0; i < nf; i++) {
                const float g = sb_rf32(pd + i*4u);
                if (g != nat[i] && !(g != g && nat[i] != nat[i]) && bad < 8) {
                    bad++;
                    fprintf(stderr, "[mathshadow] PSMTXMultVecArray MISMATCH [%u/%u] native=%a guest=%a\n",
                            i, nf, nat[i], g);
                }
            }
            for (uint32_t i = 0; i < nf; i++) sb_wf32(pd + i*4u, nat[i]);
            delete[] nat;
        }
    }
}

// PSMTXInverse(src, inv) @ 0x80349abc — 3x4 affine inverse, returns 0 if the 3x3
// determinant is exactly 0.0f (ps_cmpo0 vs 0), else 1. Full PS decode (every
// operand tracked through the merge10/msub interleave — no ambiguity left):
//   each cofactor  c = fmaf(a, d, -(b*c_rounded))   (ps_mul then ps_msub, fused)
//   det = fmaf(m20, c02u, fmaf(m10, c01u, m00 * c00u))   (mul + 2× ps_madd)
//   rdet = fres(det) refined once:  r2 = r*r;  rdet = -fmaf(det, r2, -(r+r))
//          (fres = Gekko estimate — Common::ApproximateReciprocal, NOT 1/x)
//   inv[i][j] = cofactor × rdet (ps_muls0, plain fmuls)
//   inv[i][3] = -fmaf(inv_i2, m23, fmaf(inv_i1, m13, inv_i0 * m03))
//               (ps_mul → ps_madd → ps_nmadd, all fused)
SUNBRIGHT_OVERRIDE(ov_PSMTXInverse, 0x80349abcu) {
    const uint32_t pm = cpu.gpr[3], pi = cpu.gpr[4];
    float m[12];
    for (int i = 0; i < 12; i++) m[i] = sb_rf32(pm + i*4);
    #define M(r,c) m[(r)*4 + (c)]
    // Unscaled cofactors, exact PS op order (mul rounds first, msub fuses):
    const float c00 = fmaf(M(1,1), M(2,2), -(M(2,1) * M(1,2)));  // f13.ps0
    const float c10 = fmaf(M(1,2), M(2,0), -(M(2,2) * M(1,0)));  // f13.ps1
    const float c01 = fmaf(M(2,1), M(0,2), -(M(0,1) * M(2,2)));  // f12.ps0
    const float c11 = fmaf(M(2,2), M(0,0), -(M(0,2) * M(2,0)));  // f12.ps1
    const float c02 = fmaf(M(0,1), M(1,2), -(M(1,1) * M(0,2)));  // f11.ps0
    const float c12 = fmaf(M(0,2), M(1,0), -(M(1,2) * M(0,0)));  // f11.ps1
    const float c20 = fmaf(M(1,0), M(2,1), -(M(1,1) * M(2,0)));  // f10.ps0
    const float c21 = fmaf(M(0,1), M(2,0), -(M(0,0) * M(2,1)));  // f9.ps0
    const float c22 = fmaf(M(0,0), M(1,1), -(M(0,1) * M(1,0)));  // f8.ps0
    float det = M(0,0) * c00;                                    // ps_mul
    det = fmaf(M(1,0), c01, det);                                // ps_madd
    det = fmaf(M(2,0), c02, det);                                // ps_madd
    uint32_t ret;
    float inv[12];
    if (det == 0.0f) {                 // ps_cmpo0 EQ → li r3,0; blr (no stores)
        ret = 0;
    } else {
        const float r  = (float)fres((double)det);               // fres estimate
        const float r2 = r * r;                                  // ps_mul
        const float rdet = -fmaf(det, r2, -(r + r));             // ps_nmsub NR step
        const float i00 = c00 * rdet, i10 = c10 * rdet;          // ps_muls0 ×6
        const float i01 = c01 * rdet, i11 = c11 * rdet;
        const float i02 = c02 * rdet, i12 = c12 * rdet;
        const float i20 = c20 * rdet, i21 = c21 * rdet, i22 = c22 * rdet;
        const float t03 = -fmaf(i02, M(2,3), fmaf(i01, M(1,3), i00 * M(0,3)));
        const float t13 = -fmaf(i12, M(2,3), fmaf(i11, M(1,3), i10 * M(0,3)));
        const float t23 = -fmaf(i22, M(2,3), fmaf(i21, M(1,3), i20 * M(0,3)));
        const float out[12] = { i00, i01, i02, t03,
                                i10, i11, i12, t13,
                                i20, i21, i22, t23 };
        memcpy(inv, out, sizeof inv);
        ret = 1;
    }
    #undef M
    if (math_shadow()) {
        if (RecompFunc orig = recomp_raw(0x80349abcu)) {
            CPUState shadow = cpu;
            orig(shadow);
            static unsigned long bad = 0;
            if (shadow.gpr[3] != ret && bad < 8) {
                bad++;
                fprintf(stderr, "[mathshadow] PSMTXInverse RET MISMATCH native=%u guest=%u det=%a\n",
                        ret, shadow.gpr[3], det);
            }
            if (ret) {
                for (int i = 0; i < 12; i++) {
                    const float g = sb_rf32(pi + i*4);
                    if (g != inv[i] && !(g != g && inv[i] != inv[i]) && bad < 8) {
                        bad++;
                        fprintf(stderr, "[mathshadow] PSMTXInverse MISMATCH [%d] native=%a guest=%a\n",
                                i, inv[i], g);
                    }
                }
            }
        }
    }
    if (ret)
        for (int i = 0; i < 12; i++) sb_wf32(pi + i*4, inv[i]);
    cpu.gpr[3] = ret;
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
