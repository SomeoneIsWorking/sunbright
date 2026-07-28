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

## 2026-07-28 (cont.) — the unit-1 black, run to ground: six textures whose guest memory is ZERO

The bisect said unit 1. Chasing it by hypothesis cost several runs and got nowhere, so the chain
from "the game said X" to "the GPU sampled Y" was instrumented link by link instead. Every step
below is a measurement.

| # | question | instrument | answer |
|---|---|---|---|
| 1 | is slot 1 itself broken? | `SBR_TEX_MIRROR=1` + `SBR_TEXMAP_FORCE=<u>` — same texture in all four slots, force each | NO. Forcing slot 0 vs slot 1 differs on 2.7% of pixels (run-to-run animation jitter) |
| 2 | which pixels change? | `SBR_TEV_VIZ=4` (stage 0's unit as a grey level) crossed with the named-vs-pinned difference | 62,698 of 67,693 unit-1 pixels darken (93%); unit-0 pixels: 304 of 218,589 |
| 3 | sample or combiner? | `SBR_TEV_VIZ=1` (the raw sample, before any TEV) | the SAMPLE is black — mean 15.8, 90.9% below 8/255 |
| 4 | coordinate or LOD? | `SBR_TEV_VIZ=5` (explicit LOD 0), `=6` (fixed uv 0.5,0.5) | neither: 17.3 / 90.8% and 16.0 / 90.9%. A fixed centre coordinate is still black |
| 5 | so what is BOUND? | `SBR_BIND_LOG=<n>` — the cache key AND its decoded mean, per slot, at bind time | slot 1 = key `0x80cfafa0` (CMPR 64x64), **decoded mean 0.0**, on every large terrain batch |

**`0x80cfafa0` decodes to zeros, and the per-draw state cannot show it** — the state reports the
DESCRIPTION the game wrote (address, format, dimensions), and that description is correct. Only the
cached image is black. That is why five sessions of comparing parsed state found nothing: the parser
was right the whole time.

Six textures decode black in a settled Delfino tick, out of 194:

```
0x80a9bd20 IA4   32x32     1024 B   zero run -38/+73
0x80abcc40 IA4   32x32     1024 B   zero run -38/+73
0x80cf0ac0 I4    128x128   8192 B   zero run  -0/+0
0x80cfafa0 CMPR  64x64     2048 B   zero run  -0/+0     <- bound to unit 1 across the terrain
0x80da3860 IA8   32x32     2048 B   zero run  -0/+1
0x80fea480 RGB565 320x224 143360 B  zero run  -0/+0     <- a CONFIRMED EFB copy destination
```

Ruled out, each by measurement, not argument:

- **Not a decoder bug.** The raw source bytes are zero (`00 00 00 ...`), so there is nothing to
  decode wrong. Other CMPR textures in the same frame decode to means of 196-244.
- **Not a first-sight caching race.** `sbr_render_recheck_black()` re-decodes every cached-black
  texture from guest memory each frame report: it is STILL zero later. The cache is not stale.
- **Not unallocated memory.** The zero run is exactly the texture's tiled size and no more — the
  byte before the buffer and the byte after it are both non-zero. These are buffers the game
  ALLOCATED and something is supposed to fill.

`0x80fea480` is the tell: it appears in the EFB texture-copy destination list
(`0x803f4440`, `0x80d0f9e0`, `0x810a5440`, `0x80fea480`), and this port parses EFB copies but never
writes the copied pixels back into guest memory. Its blackness is therefore fully explained. The
other five are the same SHAPE of problem — allocated, texture-sized, zero-filled, dynamically
written — but `0x80cfafa0` is NOT in the copy list over a 240 s run, so whatever fills it is a
different mechanism (ARAM DMA into MEM1 is the obvious candidate; the recomp has `dev_aram.cpp`).

**Next step, and it is a WRITE question, not a render one:** put a memory-write watch on
`0x80cfafa0` and find who is supposed to write those 2048 bytes. Until those buffers are filled,
`SBR_TEXMAP_UNITS` stays at 0x1 by default — pinning is explicitly NOT a fix, it is the
better-looking of two known-wrong behaviours, and the code says so.

## Landed this session

- **`GX_TG_SRTG` fed the rasterized colour**, not the raw stored CLR0 (the defect that made the
  whole frame one flat colour).
- **TEV compare mode** (`bias == 3` is not a bias), RE'd from `GXSetTevColorOp`. 537 of 2816
  enabled stages use it. Real and silent, but NOT the unit-1 black — the frame is unchanged.
- Instruments, all env-gated and kept: `SBR_DRAW_STATE=<tick>` (per-draw units, stages, combiner
  selectors, texgen config, coordinate ranges, NDC box, depth state), `SBR_TEXMAP_UNITS=<mask>`,
  `SBR_TEXMAP_FORCE=<unit>`, `SBR_TEX_MIRROR=1`, `SBR_TEV_VIZ=1..6`, `SBR_BIND_LOG=<n>`, and the
  EFB texture-copy destination log.

## Do not re-derive

- The parser is not the problem. Register-ID tables, RAS1_TREF, XF texgen decode and the unit
  binding model are all confirmed against the game's own writers (see the 2026-07-28 section above).
- Slot 1 is not broken, the coordinate fed to it is not degenerate, the LOD is not the issue, and
  the alpha test is not involved. All four were measured, not reasoned about.

## 2026-07-28 (cont.) — the in-process per-draw state oracle, and a conclusion RETRACTED

User: "why don't you do Aurora compare in the same process since recomp+Aurora renders fine". That
question falsified the finding above, and the falsification is the important part.

**RETRACTED: "the unit-1 black is six textures whose guest memory is zero".** The memory really is
zero — both read paths agree (`sb_r8` on the guest EA and the raw `g_ram_base + phys` pointer aurora
is handed give identical zero bytes, at BIND time as well as at frame end). What was wrong was the
inference. Aurora is in the SAME process, is handed the SAME pointer for the same textures, and
renders Delfino Plaza perfectly — sky, sea, NPCs, palm tree, all textured. So those zero bytes
cannot be what the terrain samples on the working path, and "the buffers are never filled" does not
explain anything. Aurora simply is not binding those addresses where this port is.

That is a state difference, and aurora is right there to be asked.

### The tool: `SBR_STATE_DIFF=<n>` (`runtime/state_oracle.{h,cpp}`)

Both sides record, per DRAW: `numTevStages`, `numTexGens`, each stage's (texmap, texcoord,
texEnable), and each unit's texture identity. This port's identity is the TX_SETIMAGE3 address;
aurora's is `texObj.texObjId`, which `emit_texobj` sets to exactly that address — so they compare
without translation. Aurora's hook is in `command_processor.cpp:handle_draw`, weak-linked over a
plain C ABI so aurora still builds standalone.

Two pairing problems, both found by the tool reporting nonsense and both fixed rather than ignored:

1. **Pairing by ordinal is wrong.** Aurora drains the FIFO a frame or two behind this parser, so the
   Nth draw overall is not the same draw. Flat-list pairing reported *98% disagreement* — the tool
   lying, not a finding. Now each side closes its own frame (this parser at the stream swap, aurora
   when the stream position restarts) and the report pairs the oldest closed frame from each.
2. **A frame's counts still differ slightly** (~24 of ~29,400): aurora's boundary is inferred from
   the position restarting and cannot see a buffer's first draws. The report compares the aligned
   prefix and states how many trailed; a divergence over 5% is refused outright.

### What it says

**829 of 29,619 draws (2.8%) disagree — and the stage tables agree EXACTLY.** Same `numStages`,
same `numTexGens`, same per-stage map/coord/enable. So RAS1_TREF, GENMODE and the texgen decode are
confirmed correct a third time, now against a live oracle rather than against the SDK source.

Every disagreement is the **texture identity on a unit**. Example (both agree stage 0 samples map1):

```
draw 21 MINE   stages=1 texgens=1 | s0:map1/c0 | units 0:8e2720 1:b0ffa0 2:3db200 3:3db200
draw 21 AURORA stages=1 texgens=1 | s0:map1/c0 | units 0:a86140 1:f6ce20 2:a7c240 3:871680
```

**Do not act on that yet — the instrument is not validated.** Aurora's units 1-3 read as the SAME
three ids (`f6ce20`, `a7c240`, `871680`) in every draw reported, and its unit 0 appears to trail
this port's by one draw. Constant output is the classic signature of an instrument that cannot show
the other answer (CLAUDE.md: validate against a known-positive first). Either
`g_gxState.textures[m].texObj.texObjId` is not where aurora holds the bound texture at draw time, or
the frame pairing is still off by one. **Validate that before believing any of it** — feed the
oracle a case where the units MUST differ and check it says so.

Next: validate the aurora-side field, then read off which unit identity is wrong and why.

## 2026-07-28 (cont.) — the oracle, validated and fixed: the parsed state is NOT the problem

### Validating the instrument first

Aurora's units 1-3 looked constant in the first report, which is how a broken instrument looks. The
known-positive check — count DISTINCT unit ids per side over a whole frame — clears it:

```
u0: mine 97  aurora 108     u1: mine 40  aurora 39
u2: mine 11  aurora  8      u3: mine 10  aurora  7
```

Aurora's units vary as much as this port's, so `textures[m].texObj.texObjId` IS where it holds the
bound texture. The "constant" impression came from sampling only the FIRST few disagreeing draws,
which are the 2D overlay — whose upper units are legitimately untouched. The report now samples
across the frame instead of taking the head.

### Two real fixes found on the way

**BP WRITE MASK (register 0xFE) was ignored entirely.** GX's BP is not write-only: a write to 0xFE
arms a 24-bit mask for the NEXT register write, and only the masked bits are updated — the rest keep
their previous value, then the mask resets. `GDGeometry.c:GDSetGenMode2` arms `0x07FC3F` before
writing GENMODE; `GDSetCullMode` arms `0xC000` and writes a value whose other 22 bits are zero. This
parser applied the raw payload, clobbering every field outside the mask. **Measured: 3.5 MILLION
mask writes per report — more than any other register.** Aurora has implemented it all along
(`merged = (cached & ~mask) | (value & mask)`), and its own comment notes a genMode write that sets
only bit 15 merging with a cached bit 14. Implemented here to match.

**Pairing draws by ordinal was wrong, and it was manufacturing findings.** Aurora records ~24 fewer
draws per frame (its frame boundary is inferred from the stream position restarting), and that
deficit accumulates THROUGH the frame, so late draws were compared against unrelated draws. Now both
sides carry the draw command's byte OFFSET in the frame's FIFO stream — aurora replays exactly the
buffer this parser emits, so its `cmdPos` is that offset — and draws are paired by it.

### The result, and it redirects everything

| pairing | draws disagreeing | of which lag-like | genuinely different |
|---|---|---|---|
| by ordinal | 803 of 29,365 (2.8%) | 311 | 492 |
| **by stream offset** | **134 of 29,492 (0.45%)** | **133** | **1** |

**The parsed state is essentially IDENTICAL to aurora's.** 99.55% of draws agree exactly on
`numTevStages`, `numTexGens`, every stage's (texmap, texcoord, texEnable) AND every unit's bound
texture. Of the 134 that differ, 133 have a unit id that appears on aurora's side within four draws
— a small ordering skew in when a bind takes effect, not a different texture. Exactly one draw per
frame carries a genuinely different texture.

So the earlier retraction was itself half wrong, and the correction matters: the "2.8% disagree,
every disagreement is texture identity" reading was an artefact of the pairing. The stage tables DO
agree, as the first (head-sampled) report suggested.

**This rules out the whole family of hypotheses this arc has been chasing.** The FIFO parse, the
unit binding, the TEV stage table and the texgen configuration are all correct — confirmed against a
live oracle rendering the same stream correctly in the same process. Whatever makes the native frame
differ from aurora's is DOWNSTREAM of the state: the vertex frontend (transform, lighting, texgen
evaluation), the TEV evaluation in the shader, or the texture decode/upload — not the state
extraction.

**And it sharpens the black-texture question rather than answering it.** If this port binds the same
texture ids aurora does, then aurora binds `0x80cfafa0` too and renders it fine, from the same zero
bytes. So the difference is in what each side DOES with that binding — aurora's texture upload for
that id, versus this port's decode. That is the next thing to instrument, and it is now a narrow
question about one id rather than a search.

## 2026-07-28 (cont.) — ~~ROOT CAUSE~~ FALSIFIED: the capture seam is CORRECT (my lookup was backwards)

The oracle proved the FIFO-derived state matches aurora's at 99.55% of draws. That says nothing
about the state the RENDERER uses, because the renderer does not read the FIFO state directly:
`overrides/j3d_capture.cpp:257` snapshots it at `J3DShape::draw`, from the CPU side. Extending the
oracle to that third consumer — stamping each snapshot with the parser's stream position and
comparing it against the first FIFO draw at or after that position — measures the assumption:

```
capture seam: 435 of 929 snapshots disagree with the FIFO state at their own stream position

capture@12641 SEAM  stages=1 texgens=1 | s0:map0/c0 | units 0:a86140 1:3db200 2:3db200 3:3db200
capture@15424 FIFO  stages=1 texgens=1 | s0:map1/c0 | units 0:8e2720 1:b0ffa0 2:3db200 3:3db200
```

**47% of drawables carry a material that is not theirs.** The snapshot is taken at stream offset
12641; the drawable's own first draw command is at 15424 — **2,783 bytes later**. Everything the
material writes in between, the texture binds included, happens AFTER the snapshot. Note the shape
of the error: the seam reports `map0` with unit 1 holding the J3D 4x4 null (`3db200`), while the
stream at the drawable's own draw says `map1` with a real texture on unit 1. That is precisely the
"stages name units 1-3 but those units look unbound" symptom this arc opened with — and it was never
a binding problem at all.

This explains every observation in this arc at once, including the ones that contradicted each
other:

- The parse is correct (three independent confirmations: the SDK source, aurora's live state, and
  the register tables) — yet the frame is wrong.
- Aurora renders correctly from the same stream and the same guest memory, because aurora uses the
  state at each draw rather than a snapshot taken earlier.
- Unit 1 blacked the scene: the seam hands the renderer a unit-1 binding from the WRONG material,
  so routing stages to the unit they name samples a texture that material never bound. Pinning to
  unit 0 looked better only because unit 0 is rebound constantly and is therefore less often stale.
- The six "zero memory" textures are real but were never the cause. They are bound at some point in
  the frame; the seam simply attributes them to drawables that do not use them.

**The fix is not to move the snapshot earlier or later** — there is no correct moment for it,
because one CPU-side seam cannot describe a stream position it has not reached. State has to be
associated with the DRAW, which is what the renderer doctrine already says the frontend should do
(driven from `dev_gxfifo`, not from an sms-boot-style capture). The smallest correct step: have the
capture seam register the drawable as PENDING, and let the FIFO parser attach the material state
when it reaches that drawable's own draw commands. Geometry keeps coming from the seam (it needs
J3D's matrix/skinning knowledge); material state comes from the stream, at the right position.

`SBR_STATE_DIFF=<n>` now covers all three consumers: FIFO parse, aurora, and the capture seam.

### The fix attempt: right mechanism, wrong pairing (SBR_MATERIAL_FROM_DRAW=1, default OFF)

Implemented the obvious consequence: the seam registers a drawable as PENDING and the FIFO parser
attaches the material state when it reaches a draw command. Result, and both halves matter:

- **The missing scenery came BACK.** Sky, sea, the tower, palm trees, awnings, plants — everything
  that had been black under `SBR_TEXMAP_NAMED` renders. That is strong evidence the mechanism is
  right: state does belong to the draw, and taking it from there is what makes those materials
  resolve at all.
- **The textures land on the WRONG surfaces.** The plaza ground is paved with window and building
  textures. edgeIoU 14.9% / lumaCorr +0.486, against 23.3% / +0.613 for the seam snapshot.

The reason is a pairing assumption that does not hold: **J3D sorts packets into draw buffers**, so
the order this seam traverses shapes in is NOT the order they reach the FIFO. "The next draw command
after this drawable was registered" is therefore a different shape's draw, and the material attaches
to the wrong geometry — a different wrong answer from the seam's, not a fix.

So the mechanism is kept and gated OFF (`SBR_MATERIAL_FROM_DRAW=1`), with the default path unchanged
and re-verified at 29.0% / +0.719. What it still needs is an IDENTITY linking a stream draw to the
drawable it belongs to, rather than an ordering assumption. Candidates, cheapest first: hook the
point where a shape's own display list is REPLAYED (rather than where the traversal visits it), so
the seam runs at the emission position; or match a stream draw to a drawable by its vertex
signature (count plus matrix indices, both of which the seam already decodes).

Note what the metric did here: the frame gained a whole missing background and its score went DOWN,
because misplaced textures cost more than absent geometry. The image was the evidence; the number
would have sent this the wrong way, again.

### RETRACTED — the "47% of drawables carry the wrong material" finding was my own instrument bug

The correlate was backwards. `ov_shape_draw` runs the real `J3DShape::draw` FIRST (deliberately — it
needs the matrix loads the draw itself issues, see the comment at `j3d_capture.cpp:137`), so by the
time the snapshot is taken **this shape's draw commands are already in the stream**. The correct
correlate is therefore the LAST draw at or BEFORE the snapshot position, not the first at or after.
Looking forwards compared each shape's snapshot against the NEXT shape's state, which is exactly the
one-shape shift the "fix" then baked in — and exactly what the misplaced textures in that frame were.

With the lookup corrected:

```
capture seam: 0 of 936 snapshots disagree with the FIFO state at their own stream position
capture seam: 0 of 953 snapshots disagree
capture seam: 0 of 902 snapshots disagree
```

**Zero.** The capture seam is correct for every drawable in every frame measured. The
`SBR_MATERIAL_FROM_DRAW` machinery is DELETED rather than left gated: its premise is falsified, and
a dead mechanism kept "just in case" is the tombstone this project bans. Default path re-verified at
29.0% / +0.719.

### Where that leaves the arc — all three state consumers are now confirmed correct

| consumer | verdict | evidence |
|---|---|---|
| FIFO parse | correct | matches aurora on 99.55% of draws, paired by stream offset |
| aurora | reference | renders the plaza correctly from the same stream and memory |
| capture seam snapshot | correct | 0 of ~900 disagree with the FIFO state at the shape's own draw |

So the state the renderer consumes is right, and every remaining hypothesis about the state is dead.
The unit-1 black is DOWNSTREAM of the state: in what the renderer does with it — the texture
decode/upload keyed by that state, or the shader's evaluation. That is a much smaller search space
than this arc has been working in, and it is where the next iteration goes.

One fact from the failed fix is still worth keeping, because it is strange and unexplained:
**attaching the NEXT shape's material made the missing scenery RENDER.** Geometry that is black with
its own (correct) material draws with a wrong one. Whatever blacks those surfaces is therefore a
property of their own material — a stage, a konst, a texture — not of the geometry or the binding.

### A note on the instrument

Two instrument bugs in one session (ordinal pairing, then a backwards correlate), each of which
produced a confident, specific, wrong finding — and the second one got as far as a committed "ROOT
CAUSE" and a code change. Both were caught the same way: the fix did not do what the finding
predicted. The lesson is not "be more careful", it is that **a state oracle needs its own
known-positive** — for a correlate, that means checking it against a case whose answer is known
before believing a number it produces.

## 2026-07-28 (cont.) — stop black-boxing it: the pixel pipeline is now testable and self-explaining

User: "This is too much blackbox debugging, need to make these understandable first." Correct, and it
is the project's own rule (TOOLING / VERIFICATION FIRST; unit-test-from-RE before whole-system
checks). Everything above ran the whole game, scored whole frames and inferred backwards through a
30,000-draw stream — which is how two confidently-wrong findings got as far as commits in one day.

The pixel pipeline existed ONLY as a GPU shader. It could not be asked a question: no test, no way
to evaluate one material, no way to see why a surface was black except by rendering a frame.

### What now exists

**`runtime/tev_eval.{h,cpp}` — the TEV pipeline as plain C++, and the REFERENCE definition of it.**
Stages, arg selectors, bias/scale, compare mode, konst, the alpha test. `geom.frag.glsl` mirrors it;
where they disagree, tev_eval is the definition, because it is the one with tests. `sbr_tev_konst`
moved here out of the SDL-linked renderer — keeping it there is what would force a unit test to link
a GPU backend.

**`tests/tev_eval_test.cpp` — expectations hand-derived from the SDK, not from a run.** Every case
cites where its expected value comes from (`GXSetTevColorOp`'s packing, the `GXTevOp` enum, the
konst ramp, `GXCompare`). Six groups: the lerp form with bias/scale/subtract, compare mode across
all four widths, register chaining, a disabled stage sampling nothing, the alpha test including a
BAND, and the konst ramp. `ctest` / `./build-sms-recomp/tev_eval_test`.

**Negative control, because a suite that passes on first write is exactly when to distrust it.**
Disabling compare-mode handling makes 7 checks fail with specific values; restoring it makes them
pass. The suite can see the other answer.

**`SBR_TEV_TRACE=<tick>` + `SBR_TEV_TRACE_BLACK=1` — a real drawable explains itself.** Evaluates the
drawable's actual state against its actual decoded texels (sampled with the coordinate the stage
naming that unit really uses) and prints every stage's inputs, its output and its destination
register, plus the colour-channel configuration that produced RAS. `_BLACK=1` makes it self-select:
evaluate every drawable, print only the ones that come out black. No frame score, no GPU, no
inference.

### What it said, immediately

```
drawable 1 verts=912 numStages=5 ras[0.000 0.000 0.000 a1.000]
  unit 0 = 0x80868360 256x256 fmt14 texel[0.322 0.094 0.094 a1.000]
  unit 1 = 0x80870360 64x64  fmt0  texel[0.733 0.733 0.733 a0.733]
  chan0: light=1 matSrc=reg ambSrc=reg diffFn=1 attn=1/1 mask=0x03 numChans=2
  chan0: matReg[1.000 1.000 1.000 a1.000] ambReg[0.502 0.502 0.502]
  light 0: col[1 1 1] pos[-193500 443523 -308437] cosAtt[1 0 0] distAtt[1 0 0]
  light 1: col[1 1 1] pos[12154 -1537 -9533]      cosAtt[1 0 0] distAtt[1 0 0]
  stage 1: map0 -> c[0.322 0.094 0.094]->reg0
  final [0.000 0.000 0.000 a1.000]
```

**The textures are fine. The rasterised colour is BLACK.** Every stage that reads RASC therefore
produces black no matter how well anything decoded — which is why chasing texture identity for a day
found nothing wrong with it: there was nothing wrong with it.

And the channel cannot legitimately be zero: `acc` starts at the ambient register, 0.502, and the
material register is 1.0, so the floor is 0.502. It reaches zero because `diffuseFn = 1` is
**GX_DF_SIGN — the dot product is used UNCLAMPED** — and with two lights whose contributions are
negative for a surface facing away, `0.502 + diff0 + diff1` goes below zero and clamps to black.

That is now the narrow question, and it is a `GXSetChanCtrl` decode question rather than a rendering
one: are `diffuseFn`, `attnEnable` and `attnSpot` being read from the right bits? There is precedent
— memory `[[mario-paleness-attnfn-decode-swap-2026-07-15]]` records aurora's XF chanctrl `attnFn`
bit 9 / bit 10 being SWAPPED, found in this same register. Next step is to RE `GXSetChanCtrl` from
the SDK and check this parser's decode against it, the same way the TEV op encoding was checked.

### The answer, one run later: the stages ask for colour channel 1, which this port never computes

Extending the same trace to print the per-light working and the per-stage RAS channel selector:

```
drawable 1 verts=912 numStages=5 ras[0.000 0.000 0.000 a1.000]
  rasChannel per stage: s0=7 s1=7 s2=7 s3=0 s4=1
  chan0: light=1 matSrc=reg ambSrc=reg diffFn=1 attn=1/1 mask=0x03 numChans=2
  chan0: matReg[1 1 1] ambReg[0.502 0.502 0.502]
  vertex0: model normal[-0.826 0.498 0.266] (len 1.000) -> view normal[0.031 -0.234 0.972]
    light 0: dist=573241 ldir[-0.338 0.773 -0.537] atten=1.000 diff=-0.713 -> acc [-0.211 ...]
    light 1: dist=15107  ldir[ 0.804 -0.119 -0.582] atten=0.004 diff=-0.513 -> acc [-0.213 ...]
  chan0: mat[1 1 1] * acc = [0.000 0.000 0.000]
  stage 4: map7(off) -> c[0.000 0.000 0.000]->reg0
```

**Stage 4 — the last stage, the one that writes PREV and decides the pixel — reads RAS channel 1.**
`numChans = 2`, so the game really is running two colour channels. This port computes only channel 0
(`light_channel(d.xf, 0, ...)`), and the shader ignores the per-stage channel selector entirely and
always feeds `v_col`. So the final stage of every such material is handed channel 0's value instead
of channel 1's — and for these surfaces channel 0 is legitimately black, because the sun is behind
them and `GX_DF_SIGN` is unclamped, which is correct GX behaviour for channel 0.

That is why the wrong material made them RENDER: a different material's stage 4 happened not to read
channel 1.

**This was in this file's own "Still missing" list from the start** — "colour channel 1 and the alpha
channels (computed, unused)". It was never connected to the black because there was no way to see
which channel a stage asked for. One trace run made it obvious.

### The work this names, in order

1. **Evaluate colour channel 1** (`d.xf.chan[1]`, its own material/ambient registers at XF
   `0x100B`/`0x100D` — mapping confirmed against `GXSetChanAmbColor`/`GXSetChanMatColor`, which write
   `colIdx + 10` and `colIdx + 12`), and carry it to the shader as a second interpolant.
2. **Honour the per-stage RAS channel selector.** RAS1_TREF bits 7..9 already parse into
   `SbrTevStage::rasChannel`; the shader must select on it — 0 = colour0, 1 = colour1, and 7 = the
   constant ZERO, which currently receives channel 0's colour by accident.
3. **Specular attenuation is computed as spotlight.** `GXSetChanCtrl` encodes bit 9 =
   `attn_fn != GX_AF_NONE` and bit 10 = `attn_fn != GX_AF_SPEC`; this parser stores those as
   `attnEnable`/`attnSpot`, which is faithful, but `light_channel` branches only on `attnEnable` and
   always runs the SPOT formula. A GX_AF_SPEC channel therefore gets the wrong attenuation. Aurora
   fixed exactly this bit pair once before (see its comment in `command_processor.cpp` and memory
   `[[mario-paleness-attnfn-decode-swap-2026-07-15]]`).
4. **`cosA` is negated here and not in aurora** (`cosine = max(0, dot(ldir, light.dir))`). Inert in
   the plaza because every light's `dir` is zero, so it is UNVERIFIED rather than known-wrong — the
   SDK's `GXInitLightDir`, which would settle whether the stored direction is pre-negated, is not in
   the decomp (inlined away). Needs a test case with a real spotlight before it is touched.

Each of these is now a unit-testable statement about a documented encoding, not a hypothesis about a
frame.
