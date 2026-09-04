# The sea wash: direct attribution instead of bisection (2026-07-22)

Recomp renders the file-select/title sea washed near-white; the oracle (decomp+aurora, same
renderer) does not. Measured on the sea rect (150,640)-(470,800):

| run | near-white | mean |
|---|---|---|
| recomp baseline | 82.1% | (230,227,228) |
| oracle | 0.0% | (80,189,205) |

## The instrument

`SB_PIXEL_WATCH=x,y[,frame]` in aurora `draw_prim` transforms every prim's vertices through the
same matrices the GPU uses (per-vertex PNMTXIDX -> position matrix -> projection), divides by w,
and maps to screen pixels via `logicalViewport`. Each prim is classified:

- **COVERS** — the screen bounding box contains the watch point.
- **CROSSES** — at least one vertex has clip `w <= 0`. The box is *not* a valid coverage answer:
  the clipped polygon can smear far outside the box of the vertices in front, and a prim with
  every vertex behind the eye produces no box at all.

`SB_SKIP_COVERING=1` drops COVERS prims, `=2` drops COVERS + CROSSES. Both skip **in `draw_prim`,
before the vertex push, with `pos` still advanced** — a prim that merges into the previous draw
returns before ever reaching `push_gx_draw`, so a skip at that later site silently misses the
merged majority, and a skip that does not advance `pos` desynchronizes the FIFO parse.

## Results

1. `SB_SKIP_COVERING=1` — **393,001 covering draws dropped, sea UNCHANGED (82.2%)**.
   No prim whose transformed box contains the pixel paints it.
2. `SB_SKIP_COVERING=2` — sea **0.2% near-white, mean (22,59,64)**. The wash is produced by prims
   in the CROSSES class. (Too dark, because legitimate sea geometry was dropped along with it.)
3. Narrowing =2 by blend pair: `SB_SKIP_WNEG_BF=1,3` and `=1,5` **each independently** give
   0.2% / (22,59,64) — byte-identical. Two disjoint subsets cannot each be the sole cause;
   either the filter is not discriminating or the wash is produced redundantly by many draws.
   **Unresolved — do not build on result 3.**

### Negative controls (both refute "removing enough draws clears anything")

- 393,001 covering draws dropped -> wash unchanged.
- 640,401 opaque perspective 4-vertex quads dropped (`SB_SKIP_OPAQUE_P4`) -> wash unchanged.

### The oracle also has crossing prims

7,239 CROSSES prims in a sampled oracle title frame, and it renders correctly — so `wneg > 0`
per se is NOT the defect. The classes differ in kind: the oracle's dominant crossing family is
`bf=1/0` (opaque) with `wneg == vtxCount` (entirely behind the eye, correctly culled); the
recomp's at the watch pixel are `bf=1/3` (ONE/INVSRCCLR, 64x64) and `bf=1/5` (ONE/INVSRCALPHA,
256x256) with `wneg = 1` — *partial* crossings, which are the ones that smear.

## CORRECTION: the crossing-class findings above are SUPERSEDED

Results 2 and 3 (the CROSSES skip) do **not** localize anything. The control that killed them:
applying the same partial-crosser skip to the **oracle** takes its sea to (3,1,1) — near black.
Partial eye-crossers carry essentially the whole scene in both runtimes, so removing them
removes the picture. "The wash is painted by partial crossers" reduces to "the scene is drawn
by the scene."

Result 3 (two blend-pair subsets each clearing the wash) was an instrument bug, as suspected:
`blendFacSrc/Dst` are **stale whenever `blendMode == GX_BM_NONE`**, so a filter matching on
them without checking `blendMode` sweeps in opaque draws and two "disjoint" filters select
overlapping sets. With `blendMode == GX_BM_BLEND` required, dropping the entire screen-blend
family (`SB_SKIP_BF=1,3`, 2,000,501 draws) and the entire `1/5` family (812,501 draws) each
leave the wash **completely unchanged**. Both families are exonerated.

## The instrument, fixed properly

A bounding box over the in-front vertices is not a coverage test, and no amount of extra
classes repairs that. `draw_prim` now computes **true coverage**: triangulate the primitive
(QUADS/TRIANGLES/STRIP/FAN), clip each triangle against the near plane `w >= eps` with
Sutherland-Hodgman in homogeneous space, project the clipped polygon, and do an exact
point-in-triangle test. There is no CROSSES class any more — a straddling prim is simply
clipped and then answered exactly, like the GPU does.

Validation: `SB_SKIP_COVERING=1` with clip-correct coverage takes the sea from 82.1% near-white
to **0.2%**, where the bounding-box version dropped 393,001 draws and changed *nothing*.
The instrument now both names and removes the painting draws.

### Controls that make the instrument trustworthy

- **Sham blend pair** (`SB_SKIP_WNEG_BF=13,13`, a pair nothing uses) → 82.1%, unchanged.
  The filter is live, not inert.
- **Sham surgery** (drop only prims with *every* vertex behind the eye — GPU-clipped, so
  visually inert) → 82.1%, byte-identical mean. The skip mechanism does not perturb unrelated
  rendering, so skip results are not contaminated.
- 393,001 box-covering draws and 640,401 opaque perspective quads each dropped with no change:
  "removing enough draws clears anything" is false.

## ⚠️ THE PREMISE WAS WRONG — there is no "wash", and the reference frame was a DIFFERENT SCENE

Two errors, found only by **looking at the images** instead of at the numbers:

1. **The "oracle" reference was the TITLE SCREEN.** `SB_STAGE=15` boots to the title, not to
   file-select — reaching file-select needs `SB_PAD_SCRIPT="800:START 840:-"` and a much later
   dump. Every oracle number in this file above (0.0% near-white, mean (80,189,205)) was the
   title screen's open ocean, compared against the recomp's *file-select*.
2. **The measurement rect (150,640)-(470,800) sits on the white SURF LINE** where the waves
   break on the beach — legitimately white. "82.1% near-white" was measuring foam and calling
   it a defect. The recomp's file-select otherwise renders correctly: sky, sea, sand, Mario,
   palm, menu, HUD all present and right.

With the oracle actually at file-select and the cameras visually matched, the covering-prim
inventories at the same pixel are **nearly identical**, and the claimed opaque deficit
evaporates — it was entirely the title-vs-file-select confound:

| covering prims at the pixel | oracle (file-select) | recomp (file-select) |
|---|---|---|
| total | 83 | 71 |
| screen-blend `64x64 bf=1/3` | 41 | 41 |
| opaque base `256x256 bm=0` | 7 | 6 |
| `256x256 bf=1/5` | 9 | 3 |

## The REAL residual difference (visual, both at file-select)

- The oracle's sea meets the sand cleanly with no white foam; the recomp draws a broad white
  surf band along the whole shoreline.
- The three shadow decals under the save-file blocks are dark grey in the oracle and render as
  bright white-blue patches in the recomp.

Since the draw population and every blend/TEV *configuration* signature match, the divergence is
in what those draws **write** — bound texture content, TEV konst/register colours, or vertex
colours — not in which draws exist or how they are blended. An unbound or white texture is the
obvious candidate and has bitten this project before (`texObjId=0` cache miss; NULL-texMap TEV
callsites). That is the next thing to check, and it is a *cosmetic* residual, not a wash.

**Method lesson, the expensive one:** the entire investigation ran on a numeric comparison of a
rect chosen without ever looking at what was inside it, against a reference never confirmed to be
the same scene. Two five-second image looks would have prevented all of it. Look at the picture
before measuring the picture.

## Falsified along the way


- "The sea draw paints the wash" — `SB_SKIP_VERTS=52` proved it does not.
- The extra alpha draws, the fader family (123,401 draws), the additive scene quad, and the
  opaque perspective quads: each skip verified to fire, none changed the wash.
- `SB_QUAD_RECT`'s "no large ortho quad" conclusion is **unsound** — it applied neither the
  position matrix nor the projection.

## Harness defects found and fixed

- Behind-eye vertices were `continue`d silently, so a prim could be reported as not covering the
  point while its clipped polygon crossed it. Now counted and surfaced as CROSSES.
- Indexed positions were read big-endian unconditionally, ignoring `arr.le`. Dormant for the
  recomp (guest arrays are BE) but wrong the moment the harness is pointed at a host-endian
  runtime — which is exactly the oracle A/B this investigation depends on.
- Draw indices are **not comparable between instruments or runtimes**; identification and
  causality must come from one instrument in one run, which is why the skip is keyed off the
  same `covers` verdict the report prints.

## Cross-run counts are not comparable

Each run is wall-clock bounded, so the number of frames rendered varies and cumulative draw
counts vary with it. Compare fractions within a run, never absolute counts across runs.

---

# RESOLVED (2026-07-22): mipmapped textures were bound single-level on the register path

## Root cause

`GXTexObj_::has_mips()` gated entirely on `flags` bit 0. That bit is an **aurora-ism**: it is set
only by `init_texobj_common` and by the aurora extension opcode — i.e. only when a runtime hands
aurora a texture through the SDK, which is what the decomp runtime does.

The recomp drives textures the way the hardware does: it writes **BP registers**. That path sets a
mip min-filter and a correct `mode1` max_lod, but never touches aurora's flag. So `mip_count()`
collapsed to 1 and **every mipmapped texture was bound single-level**. Minified ground-plane
surfaces then alias into bright shimmer instead of filtering down.

Measured proof before the fix, same scene and same pixel in both runtimes:

| covering draw | oracle | recomp |
|---|---|---|
| `64x64 fmt=14` sea sparkle | `mips=4 hasMip=1 maxlod=3.0` | `mips=1 hasMip=0 maxlod=3.0` |
| `256x256 fmt=14` opaque base | `mips=6 hasMip=1 maxlod=5.0` | `mips=1 hasMip=0 maxlod=5.0` |

And the recomp's own registers said mipmapped: `minf=5` (NEAR_MIP_LIN) / `minf=1`
(NEAR_MIP_NEAR), `mode0=800001d5`. Hardware state said "walk a mip chain"; aurora's flag said no.

## The fix

Real GC hardware has no "has mips" bit — whether the TX unit walks a chain is decided by
**TexMode0's min-filter field**. `has_mips()` now derives from it, which is the hardware's own
rule and agrees with the flag wherever the flag is set (`init_texobj_common` writes min filter
`0xC0` = LIN_MIP_LIN exactly when it sets bit 0, `0x80` = LINEAR otherwise). `mip_count()` still
clamps levels to what the dimensions support, so registers requesting mips a texture has no data
for cannot read past its level-0 buffer.

## Verified on real data — recomp file-select, per region

| region | before | after | oracle |
|---|---|---|---|
| surf band | (217,226,225) | **(91,188,200)** | (85,188,200) |
| open sea | (176,175,159) | **(153,168,156)** | (148,168,158) |
| sand | (214,199,193) | **(207,186,180)** | (209,185,176) |
| shadow decal | (217,212,212) | **(194,177,176)** | (194,175,170) |
| sky (control) | (49,106,183) | (49,106,183) | (48,104,184) |

Every region converged to within ~6 levels of the oracle; the sky, already correct, did not move.
Visually the recomp now matches the **retail Dolphin capture**
(`scratch/oracle/loadstate_probe/png/fsel_dolphin_end.png`): smooth turquoise water with no white
band, dark grey shadow decals, wooden OPTIONS sign present.

Retail was consulted first, on Fable's advice, precisely because the decomp is the approximation
and its water/shadow subsystems are documented-incomplete — the recomp could have been the correct
one. It was not: retail agrees with the decomp on all three residuals.

---

# Next blocker: the recomp cannot be driven past file-select automatically

Pressing A at file-select does nothing, and that is CORRECT behaviour, not a defect. In SMS's
file-select you do not select a file with a button: **Mario head-butts the file block**. The
player walks him to it and jumps. (The decomp runtime has `SB_SEL_PICK=<0|1|2>` precisely
because Mario is in a scripted `waitingStart` state and is not freely controllable headless —
see memory `fileselect-selection-to-setnextstage`.)

Verified the input path itself is fine: `SBR_LUCENT_DEBUG=pad` shows `read 1800: buttons 0x0100`,
so A is delivered; the game simply has nothing to do with it there.

The real gap is in the harness: `SBR_PAD_SCRIPT` sets **buttons only** and cannot drive the
ANALOG STICK, so no script can walk Mario to the block. Extending it to accept stick positions
(e.g. `1800:STICK=0,-1`) is the prerequisite for any automated gameplay verification in the
recomp — the same reason the decomp needed `SB_SEL_PICK`.

Confirmed working while investigating this: seagulls now render at file-select (matching retail),
and Mario reaches his sleep idle, so the scene is running its full animation set.

---

# Mario's arms are missing in the recomp: no indexed XF matrix loads (2026-07-22)

Symptom: at file-select the recomp draws Mario's shoulders but not his forearms or gloves, with a
thin white sliver at his side — geometry COLLAPSED, not culled. Retail and the decomp oracle both
draw full arms with white gloves.

## Harness defect found FIRST (and it produced a false negative)

`SB_LOG=pnzero` reported "0 zero-rotation matrix uploads". That was meaningless: aurora gates its
GX diagnostics on a **weak** `sb_log_enabled` that the hosting runtime must provide. Only the
native-reference target provided it, so in the guest runtime the symbol resolved to null and `sb_gx_log_on` returned false for
EVERY channel — every aurora SB_LOG channel was silently dead in this runtime. Fixed by giving the
guest runtime its own registry with the same semantics and `SB_LOG` specification.

Validation matters: `SB_LOG=list` announces a channel when a callsite CHECKS it, which is how the
next result was read correctly rather than as another zero.

## FALSE START: "the recomp issues no indexed matrix loads" was WRONG

I first read the silence as "the callsite is never reached, so there are no indexed pos/nrm matrix
loads" and published that. It was wrong twice over.

**The provider still was not linked.** `sb_log.cpp` went into a static runtime library,
and a weak UNDEFINED reference does not pull a member out of a static archive — nothing strongly
references `sb_log.o`, so it was never extracted and the weak symbol stayed null. `nm` showed
`w sb_log_enabled` with no address in the final binary. The provider must be compiled into the
EXECUTABLE. This is the same silent-zero failure the file was documenting, repeated one step later.

**The raw opcode census refutes the claim outright.** `SB_OPCODE_CENSUS=500` over the file-select
frame counts, in the recomp: `20=6,458,890` and `28=6,458,890` — indexed XF loads, POS and NRM
paired as expected (oracle known-positive: `20=22,888,411`, `28=22,888,411`). The recomp issues
millions of them.

## What is actually established

With the provider linked into the executable (`nm` shows `T sb_log_enabled`) and the channel
provably live (`SB_LOG=list` announces `pnzero`, 13.8M checks), the result is a MEASURED zero:

- **0 zero-rotation matrix uploads** — skinning matrices are not being zeroed.
- **6.4M indexed pos/nrm matrix loads** — the envelope-skinning path is exercised.

So Mario's missing arms are neither "matrices never loaded" nor "matrices zeroed". Both hypotheses
are refuted with validated instruments. The defect lies further along: the matrices load, but what
the arm packets do with them is wrong (candidates, untested: the envelope matrix INDEX range per
packet — this project has already hit `wEvlpMtxNum` bounds in `TMirrorActor` — or the draw-matrix
pool's endianness on the `copy_xf_data(..., !array.le)` path, where a guest-endian pool read as
host-endian yields garbage transforms rather than zeros).

## Standing lesson

Two investigations in one day were derailed by an instrument that answered "no" when it meant
"absent". Aurora's `sb_gx_log_on` now ABORTS when `SB_LOG` is set and no provider is linked: if
the user asked for diagnostics, silently delivering none is never the right answer. (That abort
path is NOT yet exercised — both runtimes now provide the symbol, so triggering it would require
deliberately unlinking the provider.)
