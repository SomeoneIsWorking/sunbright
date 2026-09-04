# Interpolated 60fps: attributing the inter-tick delta, and the shape-vs-instance tag defect

Continues `2026-07-30_aurora_60fps_lerp_design.md`. The previous commit (45b5492) closed with a
named residual — *"the translation-delta instrument points at camera CUTS being lerped as motion
(max 26911 world units in one tick)"*. **That attribution was wrong**, and the instrument that was
supposed to support it could not have distinguished the two candidates. Both are fixed here.

---

## Why the old number could not answer the question it was being asked

The paired-draw translation delta was measured on `pnMtx`, which is **model × view**. So a large
value has two completely different explanations, with opposite fixes:

* the **camera** moved (a cut — a tick with no meaningful in-between, which must SNAP), or
* the **object** moved (a pairing defect — the table returned some other object's transform).

A single number that adds them together points at neither. That is the same failure the project has
catalogued six times: an instrument comparing two things that are not the same quantity.

## The discriminator, and its control

`interp.cpp` now divides the camera out. `pnMtx = V·M`, so `M = V⁻¹·(V·M)` recovers the object's own
world transform, and the two ticks are compared in *that* frame. The report prints both numbers
side by side, plus a distribution and the worst offenders **with their tag**, so the object is
identifiable rather than merely countable. Camera motion per tick is reported as its own
distribution, because a cut is rare by definition and a mean cannot show one.

`interp::selftest()` runs the discriminator against **both** classes before the first real tick —
a 1000-unit camera move with a static object (must report object delta 0) and a 1000-unit object
move with a static camera (must report 1000). It is not optional and not env-gated.

**It failed on its first run**, and that is the point of this note: the first version never called
`begin_tick()`, so the per-tag ordinal cursor kept advancing, nothing paired, and both cases
reported no samples. Had the self-test not existed, the run would have produced a full page of
confident attribution numbers computed from zero paired draws.

## What the validated instrument found

Camera removed, the delta barely moved: **mean 47.2 → 31.9, and the max went UP (26911 → 27944)**.
The bulk of the inter-tick motion was never the camera. Camera cuts are real but rare — 3 ticks out
of 898.

The real cause was the **tag**, and it is structural. The tag was the guest `J3DShape` address, but
a `J3DShape` belongs to the shared `J3DModelData` — the model *resource*, not the instance.
`J3DShapePacket::draw` (`J3DPacket.cpp:220`) writes `unk14->mDrawMatrices = unk18` into that shared
shape immediately before calling `draw()`: it swaps the **instance's** matrices into a **shared**
object, once per draw. So every coconut, palm tree and cloned NPC of a given model collapsed into
one identity, and pairing fell back to draw ORDER within it — which culling and Z-sorting reorder
between ticks. Instance *k* this tick paired with a different instance last tick.

**Fix:** the instance identity was sitting at the seam already. The tag is now
`(shape << 32) | mDrawMatrices` — two 32-bit guest addresses composing into 64 bits with no hashing
and no collisions.

**Measured, at equal tick count (N=598, per the harness's comparable-N rule):**

| | before | after |
|---|---|---|
| object motion, mean | 48.3 | **14.9** |

## The residual, stated honestly

The composite tag is a 3.2× improvement, **not** a collapse, and the max was unchanged to the byte
(27943.911) — which is what said the story was not finished. With the distribution (936,963 draws):

| object motion (units/tick) | count | share |
|---|---|---|
| [0, 0.1) | 688,972 | 73.5% |
| [0.1, 1) | 135,009 | 14.4% |
| [1, 10) | 85,826 | 9.2% |
| [10, 100) | 18,439 | 2.0% |
| [100, 1k) | 8,555 | 0.9% |
| [1k, 10k) | 8 | — |
| [10k, ∞) | 154 | — |

So ~88% is ordinary animation, and the defect is now **~2.9% of draws above 10 units/tick** — a pose
no object reaches in 1/30 s.

The extreme tail is a separate thing and is NOT a pairing defect: every one of the worst draws is on
**tick 263**, which the camera histogram independently identifies as the run's largest cut (12,698-unit
eye step, 107.6° rotation), and all of them carry instance `0x81583878` across several different
shapes — one multi-shape model that moves with the camera, so its world position genuinely jumps
when the camera does.

## Snapping on a cut: the plumbing landed, the signal is incomplete

A cut has no in-between, so lerping across one renders a viewpoint the game never simulated. The
discriminator must come from the GAME, **not** from a magnitude threshold on the eye step: the
measured camera-step distribution has a populated middle (34 ticks in [10,100), 36 in [100,1k))
with no gap to put a threshold in, so any threshold either cuts genuinely fast motion or misses
small warps, invisibly.

The then-current camera-cut owner hooked both `CPolarSubCamera::warpPosAndAt` overloads
(US `0x800335d4`, `0x80033390`) as observe-only wrappers, and `aurora::gfx::snap_next_interpolation`
forces alpha 1 for that tick. Alpha 1 rather than skipping the pass, so the pairing table is still
filled and the tick AFTER the cut interpolates normally — skipping would leave the table holding the
pre-cut pose and merely move the artefact one frame later.

**It does not fire on this run: 0 warp calls in ~900 ticks.** That is reported with its denominator
rather than as silence, and it is not a plumbing failure — every guest call site of both overloads
went through the retired executor's override table, and 59 other overrides announced
normally in the same run. The plaza fastboot simply never warps the camera. The hook is kept because
it is the correct signal for the cases it covers (`bosseel`, `CameraJetCoaster`, `CameraChange`,
`CameraCodeControl`); it is just not the mechanism behind the cuts measured here.

### What the cuts actually are

The eye-position context around each large step (printed with three ticks before and after; the
1000-unit trigger decides only what is PRINTED, nothing branches on it):

| tick | before | after | shape |
|---|---|---|---|
| 11 | (0, 0, 0) × 3 ticks | (6269.7, 489.1, 2936.5), held | scene start — no previous camera exists |
| 165 | (6269.7, 489.1, 2936.5), static × 3 | (1068.3, 2961.4, 4543.3), then moves smoothly | transition |
| 263 | (4145.9, 8624.9, 5031.8), static × 3 | (6500.0, 612.2, −4533.2), held | transition |

All three **jump and STAY**. That rules out the alternative explanation, which had to be excluded
before anything else: `j3dSys.mViewMtx` is a single global sampled at end of tick, so a tick
rendering a second camera (a mirror or reflection pass) would hand over that camera's view and look
exactly like a cut in the histogram — but that shape jumps and RETURNS, and none of these do.

Each cut is preceded by several ticks of a *perfectly* static camera (identical to 0.1 units) and
followed by smooth motion. That is a demo/cutscene camera holding a pose and then handing over, not
a gameplay warp — consistent with the plaza intro demo that fastboot runs into.

### The camera mode: a second signal, measured before being trusted

`CPolarSubCamera::mMode` (guest +0x50, via `gpCamera` at `0x8040D0A8`) is sampled every tick and
stamped with **aurora's own tick counter**, not one derived from the present count — presents run at
two per tick under replay, so a derived index would drift silently and any correlation drawn from it
would be worthless.

Result over 891 observed ticks (9 had no readable camera, counted rather than ignored):

    tick 262: camera mode 73 -> 0     ... and that is the ONLY change in the run

`73` is `CAMERA_MODE_REPRODUCE_DEMO`; `0` is `CAMERA_MODE_FOLLOW`. The jump is at tick 263 — one tick
later, which is the correct causal order (the mode changes, the next tick's view is built with it).

So the mode field is a **zero-false-positive** signal that catches **1 of the 3 cuts**. It confirms
the shape the eye positions implied: the intro demo camera holding a pose, then handing over to the
gameplay camera.

Ticks 11 and 165 occur *while the mode is already* `REPRODUCE_DEMO`, so they are cuts **between shots
inside the demo** — which a mode field cannot see by construction. Tick 11 is additionally a startup
transient: the eye is exactly (0,0,0) for the preceding ticks, i.e. `mViewMtx` is still identity
because no camera has written it yet.

**Not wired to the snap yet, deliberately.** A signal covering one of three cuts is an improvement
rather than a tear (a snap is whole-frame, so partial coverage cannot render two viewpoints at once,
unlike partial interpolation) — but the remaining two are understood well enough to be worth doing
in one piece rather than shipping a third of the fix and calling the residual unknown.

## Next, in order

1. **The within-demo shot cut.** The demo system reproduces recorded camera keyframes; a shot change
   is a keyframe discontinuity it knows about. That plus the mode change plus the existing warp hook
   covers all three observed classes. Startup (identity `mViewMtx`) should be treated as "no previous
   view" rather than as a cut.
2. **The 2.9% mid-range mispairings** — chase via the worst-draw tags, which now name shape and
   instance.
3. Still open from the previous commit and untouched here: ~9.8% of draws are indexed perspective
   geometry not reaching the tag seam, and the replay cost holds the paced game at 22.4 ticks/s
   rather than 30.
