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

## Next, in order

1. **Snap the tick on a camera cut.** A cut has no in-between; lerping across one renders a
   viewpoint the game never simulated. The discriminator must come from the GAME (it knows when it
   cuts — stage load, camera mode change, demo transition), **not** from a magnitude threshold on
   the eye step. A threshold is unfalsifiable against genuinely fast camera motion, and the
   histogram shows a populated middle ([10,100) 34 ticks, [100,1k) 36 ticks) with no gap to put one
   in.
2. **The 2.9% mid-range mispairings** — chase via the worst-draw tags, which now name shape and
   instance.
3. Still open from the previous commit and untouched here: ~9.8% of draws are indexed perspective
   geometry not reaching the tag seam, and the replay cost holds the paced game at 22.4 ticks/s
   rather than 30.
