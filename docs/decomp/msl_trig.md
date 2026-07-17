# MSL trig + PSMTX decode notes (sinf/cosf, MultVecSR/Array, Inverse)

Decoded 2026-06-12 from the GMSE01 binary via `sunbright-recomp --disasm` plus a
paired-single operand decoder (`scratch/psdec.py`, opcode-4 field extraction —
needed because PS operand fields are unreadable by eye). Source reference:
`decomp/sms/src/PowerPC_EABI_Support/Msl/MSL_C/MSL_Common_Embedded/Math/Single_precision/trigf.c`.
Ports live in `runtime/overrides/native_math.cpp` with SUNBRIGHT_MATH_SHADOW blocks.

## sinf @ 0x8033c7e4, cosf @ 0x8033c650  — VERIFIED against disasm line-by-line

Identical skeletons; they differ only in the three result paths (parity swap +
negations). Algorithm: quadrant reduction by π/2, then a degree-9 polynomial in
the residual.

### Constants & tables (all read from guest RAM by the port)
| Where | What | Value |
|---|---|---|
| SDA2 `r2`+0xa88 (lfs) | `__two_over_pi` | 0.63661975f |
| SDA2 `r2`+0xa8c | 0.5f (the ± rounding bias) | |
| SDA2 `r2`+0xa90 | `__SQRT_FLT_EPSILON__` | 3.4526698e-4f |
| SDA2 `r2`+0xa98 (lfd) | 0x4330000080000000 int→double magic | |
| 0x803e67c8 | `__four_over_pi_m1[4]` — **RAM copy**, filled by the static initializer @ 0x8033c988 from rodata 0x803aade8 (`#pragma cplusplus on` array init) | {0.25, 0.023239374, 1.7055572e-7, 1.867365e-11} |
| 0x803ab2c4 | `__sincos_on_quadrant[8]` — (sin(nπ/2), cos(nπ/2)) pairs, indexed by (n&3)*8 bytes | |
| 0x803ab2e4 | `__sincos_poly[10]` — interleaved even=cos / odd=sin Horner coefficients; poly[9] also used by the small-frac path | |

VERIFIED base disambiguation: the small-frac path loads `[0]`/`[4]` off the
8n-indexed base 0x803ab2c4 (= on_quadrant) and `+0x24` off 0x803ab2e4
(= poly[9]) — note both `addi` immediates are sign-extended (0xb2c4 → base
0x803a_b2c4, not 0x803b_b2c4).

### Op sequence (sinf; cosf identical until the result paths)
```
z   = __two_over_pi * x                  ; fmuls
zr  = signbit(x) ? z - 0.5f : z + 0.5f   ; rlwinm. bit31 of x-bits; fsubs/fadds
n   = fctiwz(zr)                         ; truncate toward zero, saturating,
                                         ; NaN → 0x80000000
two_n = (float)(int32)((u32)n << 1)      ; rlwinm n,1,0,30 + 0x43300000 magic
                                         ; lfd/fsubs (single-rounded)
frac = x - two_n                         ; fsubs
frac = fmaf(t[k], x, frac)  k=0..3       ; 4× fmadds (FUSED) with __four_over_pi_m1
q    = (u32)n & 3                        ; rlwinm
|frac| via bl fabsf 0x8033c224; fcmpo vs eps:
  |frac| < eps  → small-frac path (the "fallback" branch at 8033c89c: bge 0x34
                  skips it; blt falls through INTO it — i.e. the branch target
                  of the fcmpo is the POLYNOMIAL path, the fall-through is the
                  near-multiple-of-π/2 shortcut)
xsq = frac * frac                        ; fmuls
```
Result paths (all chains FUSED fmadds; q0 = quad[2q], q1 = quad[2q+1]):
| | n even | n odd | small-frac |
|---|---|---|---|
| sinf | `(frac * Hodd) * q1` | `Heven * q0` | `fmaf(P9, q1*frac, q0)` |
| cosf | `Heven * q1` | `(frac * -Hodd) * q0` (fnmadds) | `-fmaf(frac, q0, -q1)` (fnmsubs) |

where `Heven = fmaf(xsq, fmaf(xsq, fmaf(xsq, fmaf(P0,xsq,P2), P4), P6), P8)`
and `Hodd` the same over P1,P3,P5,P7,P9. First chain step is `fmaf(P[k], xsq,
P[k+2])` (coefficient × xsq), the rest are `fmaf(xsq, acc, P)` — replicated
exactly in the port. fnmadds/fnmsubs = negate AFTER the fused rounding (exact,
so host `-fmaf(...)` matches).

VERIFIED: every instruction 0x8033c650–0x8033c984 accounted for; no other exits.

## PSMTXMultVecSR @ 0x8034a3b0 — VERIFIED
Per row i: `ps_mul (mi0*x, mi1*y)` → `ps_sum0` (single add of the two rounded
products) → `ps_madd out = fmaf(mi2, z, sum)`. mi3 is loaded but only reaches
the discarded ps1 lane (the `psq_l 8(r4),w=1` puts 1.0 there). No translate.

## PSMTXMultVec @ 0x8034a2d0 — VERIFIED (previous port corrected)
NOT a single fmaf chain. Per row: `ps_mul (m0*x, m1*y)` → `ps_madd` lane0 =
`fmaf(m2, z, m0*x)`, lane1 = `m3*1.0 + (m1*y)` (the mul was already rounded;
×1.0 makes the fused add a plain single add) → `ps_sum0` lane0+lane1.
The old port fused `m1*y` into the chain — one rounding short on that product.

## PSMTXMultVecArray @ 0x8034a324 — VERIFIED
Matrix pre-merged into column pairs (merge00/merge11). Per element (stride 12,
software-pipelined loop with `mtctr(count-1)` + a peeled iteration = count
elements; count<1 would underflow CTR on hardware):
```
x' = fmaf(m02, z, fmaf(m01, y, fmaf(m00, x, m03)))   ; madds0/madds1/madds0, all fused
y' = same with row 1
z' = fmaf(m22, z, m20*x) + (m23 + m21*y)             ; ps_mul, ps_madd, ps_sum0
```
Note rows 0/1 use a different (fully fused, translate-seeded) rounding shape
than the single-vector MultVec — intentional in the SDK asm, kept distinct in
the port.

## PSMTXInverse @ 0x80349abc — VERIFIED (full operand track, no ambiguity)
3x4 affine inverse. After merge10 shuffles, the 9 unscaled cofactors are each
`ps_mul` (rounded) then `ps_msub` (fused): `c = fmaf(a, d, -(b*c))` with the
exact operand assignments recorded in the port (f13/f12/f11 ps0+ps1, f10/f9/f8
ps0; their ps1 lanes are junk and unused). Determinant:
`det = fmaf(m20, c02, fmaf(m10, c01, m00*c00))`. `ps_cmpo0` vs 0.0: **exact
equality → return 0, no stores** (NaN det falls through as "not equal" and
produces a NaN matrix — faithful). Reciprocal is `fres` (Gekko estimate —
`Common::ApproximateReciprocal`, never 1/x) refined by ONE Newton step in this
exact rounding order:
```
r2   = r * r                  ; ps_mul (rounded)
rdet = -fmaf(det, r2, -(r+r)) ; ps_nmsub, r+r via ps_add
```
Entries: `inv[i][j] = c_ij * rdet` (ps_muls0, plain fmuls, scaling happens
BEFORE the translate dot products). Translate column:
`inv[i][3] = -fmaf(inv_i2, m23, fmaf(inv_i1, m13, inv_i0 * m03))`
(ps_mul → ps_madd → ps_nmadd, fused). Returns 1.

Open/uncertain: none for these five functions. One environmental assumption:
the sinf/cosf port reads `__four_over_pi_m1` from its guest RAM copy at
0x803e67c8, which is valid only after the C++ static initializer @ 0x8033c988
has run (true for any in-game call; a hypothetical pre-static-init call would
read zeros — the shadow harness would flag it).
