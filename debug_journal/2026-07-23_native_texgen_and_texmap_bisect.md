# Native SDL3-GPU render: texgen lands, multi-texmap regresses (bisected)

2026-07-23, recomp native renderer (`sms-recomp/runtime/native_render.cpp` + `scene.cpp`).

## The harness change that made this session's conclusions possible

Every earlier native-render number in this arc was a SINGLE frame's score against the aurora
oracle. That is not comparable between runs: consecutive frames differ in animation phase, and the
spread between them turned out to be **as large as the changes being measured**. Two of this
session's conclusions would have inverted on a single-frame read.

`render_compare.cpp` now accumulates every scored frame that has geometry and reports a running
**mean, best, and sample count**. Compare runs at the SAME sample count — the mean drifts as the
camera settles, so `mean over 8` vs `mean over 19` is not a comparison.

Run configuration used throughout (all numbers below are `mean over 8 scored frames`):

```
SB_HEADLESS=1 SB_W=640 SB_H=448 SB_TURBO=1 SBR_FASTBOOT=1 SBR_STAGE=1
SBR_J3D_CAPTURE=1 SBR_SDLGPU=1 SBR_TEX=1 SBR_AB=1 SBR_AB_EVERY=60
```

`SBR_J3D_CAPTURE=1` is REQUIRED — the capture seam is opt-in, and without it the scene has zero
drawables and every metric reads 0.0%. That cost two full runs to notice.

## What the scene actually uses (measured, not assumed)

Instrumented in `sbr_scene_report_zmodes`, over ~971 drawables of Delfino Plaza:

| property | distribution |
|---|---|
| max texcoord index | 0 → 428, 1 → 155, 2 → 192, 3 → 196 drawables |
| numTexGens | 1 → 242, 2 → 111, 3 → 219, 4 → 323 drawables |
| texgen source row | TEX0 → 1109, TEX1 → 538, COLORS → 641, TEX2 → 112, NORMAL → 11, GEOM → 2 |
| stage names texmap | 0 → 914, 1 → 540, 2 → 764, 3 → 196 enabled stages |
| alpha test | 334 of 971 drawables use a non-ALWAYS comparison |

**72% of drawables use more than one texture unit.** Multi-texture is the majority case here, not an
edge case — worth knowing before deciding how much vertex budget to spend. This is why the vertex
carries FOUR coordinate sets: two would have missed the largest group.

## Landed and kept

- **Alpha test** (BP 0xF3). The cutout mechanism — foliage and grates are opaque quads whose shape
  comes entirely from discarding texels. Compared on the QUANTISED 8-bit alpha, because that is what
  the hardware compares. Both comparisons and the logic op, since GX can express a band.

  **It measures as exactly neutral** (`SBR_ALPHATEST=0` → 25.2%/+0.539, on → 25.2%/+0.538) even
  though 334 drawables set a non-ALWAYS comparison. It is kept because it is what the hardware does,
  but "implemented" is not "verified working" here, and the null result is itself a lead worth
  chasing: either the metric is blind to cutout fringes at a 320x224 edge grid, or **the discard
  never fires**, which would mean the alpha reaching the test is wrong (a TEV alpha chain that
  always resolves to 1.0 would look exactly like this). Next step is a discard counter, not another
  feature.
- **Per-texture wrap/filter** (TX_SETMODE0, BP 0x80+i / 0xA0+i). Wrap is a property of the MATERIAL,
  not of the texture data — the same image is legitimately clamped by one material and repeated by
  another — so it keys the batch, not the texture cache.
- **Texgen** (XF 0x1040+i, matrices at XF 0x0078-0x00EF, MatrixIndexA/B at 0x1018/0x1019). Evaluated
  per vertex on the CPU beside the lighting, because the sources include position and normal and the
  matrices animate per frame. The colour texgen TYPES (COLOR0/COLOR1) bypass the matrix entirely and
  emit the channel's red/green directly — 641 of 2413 texgens, so getting that wrong is not minor.

## The regression, and the bisect that located it

Texgen and multi-texmap landed together and the pair scored **worse than neither**. Two independent
diagnostics (`SBR_TEXGEN=0`, and the texmap pin) turned one confusing number into four:

| config | edgeIoU@8 | lumaCorr@8 |
|---|---|---|
| neither (baseline) | 25.1% | +0.535 |
| **texgen only** | **25.1%** | **+0.544** |
| **named texture units only** | **16.9%** | **+0.335** |
| both | 16.6% | +0.336 |

Texgen is neutral-to-slightly-positive. **Multi-texmap binding is entirely responsible.**

Without the bisect the honest reading of "24.1% → 17.6%" would have been "texgen is wrong", and the
correct mechanism would have been reverted while the actual defect survived.

## The texmap regression is LOCATED but NOT root-caused — read this before re-deriving

Three measurements, in the order they were taken, because two of them nearly produced a wrong
conclusion and the third is still not sufficient.

**1. Texture variety per unit (suggestive, NOT evidence).** Per tick:

| unit | distinct texture addresses | enabled stages naming it |
|---|---|---|
| 0 | 95 | 914 |
| 1 | 34 | 540 |
| 2 | **11** | 764 |
| 3 | **9** | 196 |

I read this as "the upper units are stale". **That inference was not justified**: a unit holding few
distinct images is equally consistent with a SHARED light/environment map bound once and reused,
which is normal GX programming. Low variety does not distinguish stale from shared.

**2. Bind rate per unit (falsified the staleness story).** Counting BP writes directly
(`sbr_gxfifo_report_bp_writes`, TX_SETIMAGE3 = 0x94+m): units 0/1/2/3 receive 97779 / 50530 / 35803 /
30657 writes. The upper units are rebound constantly. They are NOT going unbound, so "the parser
never sees the bind" is dead.

**3. Bind LAG per unit (informative, still not decisive).** Each unit bind is stamped with a global
counter; per drawable, how many binds separate that unit's last bind from the newest of any unit:

| unit | distinct addrs | bind lag mean | max |
|---|---|---|---|
| 0 | 82 | 1.3 | 3 |
| 1 | 30 | 7.7 | 35 |
| 2 | 10 | 9.2 | 43 |
| 3 | 8 | 12.3 | 42 |

Unit 0 is bound as part of each drawable's own material (lag ≤ 3 = the size of one material's bind
burst). Units 1-3 were last bound several materials earlier. **This still does not settle it** — a
large lag is exactly what a legitimately persistent shared texture looks like too. Do not record
this as the root cause; it is a narrowing, not an answer.

Also ruled out: no enabled stage names an unbound unit (0 of 2414), and no stage names a unit above
3, so the `& 3` mask loses nothing. The shader-side selectors were re-derived against the hardware
field layout and match.

**Next step — and it should be the FIRST step next time.** Stop comparing pixels at the end of the
pipeline and compare state at the point of use, against aurora, per draw: for each J3DShape draw,
diff my parsed (per-stage texmap, per-unit bound texture, texcoord) against aurora's for that same
draw. This is precisely the move that solved the matrix problem earlier in this arc — a whole-frame
score can say something is wrong but never WHERE, so every step becomes a hypothesis that costs a
run to falsify. Four runs went into narrowing this one and it is still open; a state-level oracle
would have answered it in one.

**Status: the named path is implemented and OPT-IN (`SBR_TEXMAP_NAMED=1`), pinned to unit 0 by
default.** Pinning is NOT a fix and is not recorded as one — it is the better-scoring of two
known-wrong behaviours while the binding desync is found. Do not delete the named path.

## Dead ends / do not re-derive

- Sizing multi-texture support from "probably one or two units" — measured, it is four.
- Zeroing coordinate sets above `numTexGens`: an invention, not a GX behaviour. GX keeps texgen
  state for all units regardless; `numTexGens` only bounds what is rasterised.
- Comparing runs at different `mean over N` counts. The mean drifts with the camera; only equal N
  is a comparison.

## Still missing in the native path

Specular (only the diffuse/spot path exists), colour channel 1 and the alpha channels (computed,
unused), indirect TEV stages, the fog block, EMBOSS texgen, per-vertex TEXnMTXIDX (the texgen matrix
slot is taken from the XF register, so a shape supplying it per vertex uses the wrong matrix), and
C4/C8 textures (the TLUT's main-memory address is not tracked — GX writes it via the TLUT load at
BP 0x64/0x65, which this parser does not follow).
