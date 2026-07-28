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

**4. Texture decode ruled out (two cheap falsifications).** A declined texture binds WHITE, which
would wash out exactly the surfaces the upper units feed, so this looked promising:
- No C4/C8 textures appear in the scene at all and no "no decoder for format" error ever fires.
  Formats present: I4 58, CMPR 60, IA8 18, I8 10, RGB5A3 9, IA4 8, RGB565 1. Every one decodes.
- I4/I8 decode alpha = intensity (`put(out, …, i, i, i, i)`), which is what GX defines, so a stage
  reading TEXA off an intensity light map gets the right value.

So the images reaching units 1-3 are decoded correctly. What is wrong is WHICH image is there.

**5. `numStages` overrun — PREDICTED, THEN FALSIFIED.** `g_tev.stage[]` is global and persistent,
and RAS1_TREF writes arrive two stages per register. If GENMODE's `numStages` is larger than the
material actually set, the loop reads stale per-stage `texmap` fields left by an EARLIER material —
which would name units that this material never bound, and would explain every measurement above:
the units are rebound often (2), the drawable's own material only freshly bound unit 0 (3), the
textures decode fine (4), and pinning to unit 0 scores better because it ignores the stale names.
It also explains the direction of the error: the upper units mostly carry intensity maps, which
TEV MULTIPLIES, so a wrong one darkens the surface — and lumaCorr fell (+0.535 → +0.335) exactly as
a wrongly-darkened scene would.
Tested by stamping each RAS1_TREF register write on the same clock as the binds and counting stages
whose register predates the drawable's own material by more than one burst: **54 of 3206 stages in
one tick, 0 of 2143 in another — 1.7% at worst.** Real, but nowhere near enough to move edgeIoU by
8 points. The hypothesis is DEAD; it is written up in full only so the next session does not
re-derive and re-test it.

## STOP. Look at the image. (2026-07-23, after the user said "this percentage hunting won't get you anywhere")

Everything below this line in the previous revision was reasoning about a whole-frame SCORE. One
look at the actual frame reframed the defect completely, in two runs, after five had failed:

- `SBR_TEXMAP_NAMED=1` does not render a "slightly darker, slightly wrong" scene. It renders
  **one flat colour over the entire frame** — while reporting `coverage=100.0%`, 885 drawables,
  146 batches. The geometry is all there and all painted; every fragment collapses to one value.
- With `SBR_ALPHATEST=0` added, the scene **comes back**, but the sky and many surfaces render
  **BLACK**.

So the chain is: stages that sample units 1-3 return **zero**, which makes the surface black; that
zero is also the alpha feeding the alpha test, so with the test enabled those fragments are
DISCARDED and the frame fills with whatever survives.

**The 16.9% edgeIoU / +0.335 lumaCorr that five hypotheses were built on was the score of a
uniformly-flooded frame.** The metric returned a plausible mid-range number for an image with no
scene in it at all, which is a damning result for the metric, not just for the hypotheses. Every
inference drawn from it below — including the "intensity maps multiply, so the scene darkens"
argument that made `numStages` look compelling — was explaining a phenomenon that does not exist.

The real question is now narrow and answerable: **why do samplers 1-3 return zero?** Candidates, in
order of cheapness: the coordinate fed to them (texgen 1-3 output, which CLAMP would resolve to an
edge texel); the SPIR-V binding decorations for `u_tex1..3` versus the slots
`SDL_BindGPUFragmentSamplers` fills; or the texture actually bound to those slots.

## Superseded reasoning below — kept only so it is not re-derived

Every hypothesis here was falsified, and the last one was argued from a metric that turned out to be
measuring a flooded frame. Read the section above instead.

### (superseded) Where that left it

| # | hypothesis | verdict |
|---|---|---|
| 1 | upper units never bound | dead — 30-50k TX_SETIMAGE3 writes each |
| 2 | upper-unit textures fail to decode (bind white) | dead — no C4/C8 present, no decoder errors |
| 3 | I4/I8 alpha wrong for intensity light maps | dead — decodes alpha = intensity, per GX |
| 4 | `numStages` overrun reads stale stage entries | dead — 1.7% of stages at worst |

What the measurements jointly say: for a typical drawable the material's TREF registers are FRESH
(it really does name units 1-2 in its current TEV setup) while only unit 0 was freshly BOUND. That
combination is legal GX exactly when units 1-2 still hold what the material wants — a persistent
shared texture. If that is what is happening, the binding is CORRECT and sampling those units should
IMPROVE the image, which it does not. So the next suspect is not the binding at all but what those
stages do with the sample: the coordinate the upper units are sampled with, or the TEV combination
itself. Note the upper units mostly carry intensity maps, which TEV multiplies — the failure
direction (everything darker, lumaCorr +0.535 -> +0.335) fits a wrong multiplier, not a wrong image.

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

---

# 2026-07-28 — the RE, and what it found

Picking up on "start with the RE necessary for native render". Every question below was answered by
reading the game's own writers in `decomp/sms`, not by inference, and then confirmed by a run.

## RE result 1 — the texture-unit binding model is CORRECT, and the TMEM worry is dead

The uncommitted note from last session guessed that GX binds a unit to a **TMEM region**
(TX_SETIMAGE1/2), so "latest SETIMAGE0 + latest SETIMAGE3 per unit" might be structurally wrong.
Reading the two binders settles it:

- `J3DTevs.cpp:loadTexNo(texmap, texNo)` — the path essentially every material here uses (J3D bakes
  it into the per-material display list) — writes **TX_SETIMAGE0, TX_SETIMAGE3, TX_SETMODE0/1** and,
  only for CI formats, `J3DGDLoadTlut` + `J3DGDSetTexTlut`. It **never** writes TX_SETIMAGE1/2.
- `GXTexture.c:GXLoadTexObjPreLoaded` writes six registers in the fixed order mode0, mode1, image0,
  image1, image2, image3. image1/image2 come from the tex REGION and describe TMEM **caching**, not
  which image is sampled.

So TMEM is inert for a port that samples main memory, and TX_SETIMAGE3 is the right bind stamp.
The register-ID tables are ground truth in `J3DTevs.cpp` and match the parser exactly:
mode0 `0x80-0x83`/`0xA0-0xA3`, image0 `0x88`/`0xA8`, image3 `0x94`/`0xB4`, TLUT `0x98`/`0xB8`.

**Sparse binding is by design:** each `J3DTevBlockN::load()` calls `loadTexNo(i, mTexNo[i])` only
when `mTexNo[i] != 0xffff`. A unit a material does not use keeps the previous material's texture.
That is what the "units 1/2/3 carry only 34/11/9 distinct addresses" measurement was seeing — it is
CORRECT GX behaviour, not a desync, and the whole "stale binding" thread was chasing nothing.

## RE result 2 — TREF and the XF texgen register decode are both correct

`JRenderer.cpp:JRNISetTevOrder` packs RAS1_TREF exactly as the parser reads it, including
`texEnable = (map != 0xff && !(map & GX_TEXMAP_DISABLE))`. `GXAttr.c:GXSetTexCoordGen2` packs XF
`0x1040+n` as projection@1, form@2, **type@4..6, sourceRow@7..11** — the parser matches. (Its
`inputForm` reads 2 bits where the register has 1; bit 3 is always zero, so it is harmless.)

## The actual defect: SRTG was fed the stored vertex colour, not the rasterized one

With the parser cleared, a per-draw state dump (`SBR_DRAW_STATE=<n>`, reporting unit bindings, the
stage table, the texgen config and each coordinate set's range at the point of use) showed the
5-stage plaza material with all four units freshly bound to real textures — and **coordinate set 3
constant at (1.00, 1.00) on every draw**. Its texgen is `type 2 / row 2 / identity`: `GX_TG_SRTG`.

`GXSetTexCoordGen2`'s `case GX_TG_SRTG` forces the source row to 2 (COLORS) and takes the **colour
channel's own rasterized output**. This port was passing the RAW decoded CLR0 instead, which is
white for any mesh without a stored colour — pinning every SRTG coordinate to one texel of the ramp
for the entire scene. Fixed: `texgen()` now takes the lit channel result, clamped to [0,1] as the
hardware does before it becomes a coordinate.

That is what "one flat colour over the whole frame" and "black sky and surfaces" were. With it
fixed, the named-texmap path renders a real, textured, recognisable plaza.

## Bisecting the residual: it is unit 1, and only unit 1

`SBR_TEXMAP_UNITS=<mask>` routes stages naming unit m to unit m only for the bits in the mask (the
rest fall back to 0). One run per bit instead of one run per theory. All numbers `mean over 40`:

| mask | units honoured | edgeIoU | lumaCorr | frame |
|---|---|---|---|---|
| 0x1 | none (pinned) | 29.0% | +0.718 | complete |
| 0xF | 0,1,2,3 | 23.3% | +0.614 | distant scenery black |
| 0x3 | 0,1 | 23.3% | +0.615 | distant scenery black |
| 0x5 | 0,2 | 28.9% | +0.718 | complete |
| 0xD | 0,2,3 | 28.9% | +0.719 | complete |

**Unit 1 alone reproduces the entire defect; units 2 and 3 are correct.** Sky, sea, the tower,
awnings, plants and umbrellas — everything DISTANT — goes black when stages sample the unit they
name for unit 1; near geometry is unaffected either way. Ruled out along the way: the alpha test
(`SBR_ALPHATEST=0` gives a bit-identical score), unit 1's texture content (`80870360` I4 64x64
decodes to mean 147, min 51 — not black; only 6 of 194 decoded textures are near-black at all), and
stages naming an unbound unit (0 of 11,563 enabled stages name a unit holding the J3D 4x4 null).

Next: why unit 1 specifically, on distant geometry only.
