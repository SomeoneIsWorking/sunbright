# 60fps is game-native interpolation, and SMS already has the seam for it

**Directive (user, 2026-08-05):** *"I don't care about a badly done 60fps, pick the correct path, it
never meant making the game fast enough, if you pick the good path and implement it then it'll
already be fast."* And: *"Interpolation should still be an option because it preserves the game's
own physics but it should be game native so effects don't jitter."*

Two things this corrects, both of which I had wrong:

1. **60fps was never a render-speed problem.** SMS's logic ticks at 30 Hz — `Application.cpp:270`
   constructs `JDrama::TDisplay(2, ...)`, and that `2` is two NTSC fields per tick, which
   `TDisplay::draw` hands to `TVideo::waitForRetrace` and thence to `sb_frame_present(2)`. At zero
   render cost the game is still 30fps. The render work I spent this session on is real and worth
   keeping, but it was never the path here.
2. **Nor is it a 60 Hz logic tick.** Setting that `2` to `1` and halving every hardcoded per-frame
   constant changes SMS's actual physics and has unbounded surface area. Interpolation is the
   conservative option precisely because it leaves physics untouched — it only adds in-between
   frames.

## Why the old lerp60 jittered — a layering bug, not a tuning bug

The retired `runtime/interp60.h` captured `J3DModel::viewCalc` matrices — the last stage before
drawing — and lerped those. Anything not expressed as a model matrix has nothing to interpolate:
JPA particles, the HUD, and above all the ghost/dash trail, whose geometry is **rebuilt each tick
from discrete 30 Hz position samples**. Mario moved at 60 Hz; the trail behind him still stepped at
30 Hz. That *relative* mismatch is the jitter the user reported. No amount of tuning the lerp could
have fixed it, because the trail had no matrix to lerp.

"Game native" is the fix: interpolate where the game knows what is moving, so a trail interpolates
*as a trail*.

## The seam already exists in the game's own code

`TMarDirector::direct()` drives phase-bitmasked perform lists in order — `mPerformListMovement`,
`mPerformListCalcAnim`, `mPerformListPreEntry`, then `mPerformListDrawBufGroup` / `Graffito` /
`Pollution` / `GX` / `Silhouette` / `GXPost`. `TLiveActor::perform(u32 phase)` decodes those bits:

| bit | work | re-runnable? |
|---|---|---|
| `0x1` | `moveObject()` | **no** — this is the physics step |
| `0x2` | `updateAnmSound()`, `mMActor->frameUpdate()`, `calcRootMatrix()`, `mMActor->calc()` | **mixed** — `frameUpdate` ADVANCES animation, the other two only recompute |
| `0x4` | `requestShadow()`, `mMActor->viewCalc()` | yes |
| `0x200` | `drawObject()` | yes |

Bit `0x2` mixing animation advance with matrix computation is the one real obstacle to running a
draw pass twice. **But the game already solves it**: `TLiveActor::performOnlyDraw` (virtual,
`LiveActor.hpp:70`) is `requestShadow` + `calcRootMatrix` + `calc` + `viewCalc` + `drawObject` with
**no `moveObject` and no `frameUpdate`** — recompute and draw, advance nothing. It is not dead
code written for this; it ships, called from `enemyAttachment.cpp`, `smallEnemy.cpp` and
`NpcBase.cpp` for actors that must render without updating.

So SMS itself already distinguishes *advance* from *draw*. The interpolation does not have to
invent that split, only drive it.

## Design

Per logic tick:

1. Snapshot each actor's `mPosition` / `mRotation` (and its animation frame) **before**
   `moveObject()` runs. That is `prev`; after movement the live fields are `cur`.
2. Run MOVEMENT + the animation advance exactly **once** — physics and animation timing are
   untouched, which is the whole point.
3. For each sub-frame at `alpha` (0.5, then 1.0): write the interpolated transform into the actor's
   live fields, call `performOnlyDraw`, present, restore.

`calcRootMatrix()` reads `mPosition`, so matrices fall out interpolated with no renderer-side
matrix capture and no draw-tag identity problem — the actor computing the matrix *is* the actor
that owns it. J3D animations already interpolate between keyframes, so a fractional animation frame
makes the skeleton interpolate as well rather than stepping.

## What is NOT solved by the above, and is the actual work

Anything whose visual state is not a function of an actor transform needs its own interpolation —
this is exactly the class that jittered before:

* **the ghost/dash trail** — geometry rebuilt per tick from discrete position samples;
* **JPA particles** — an independent simulation with its own step;
* **anything that rebuilds vertex data per tick** rather than transforming a static model.

Each needs prev/cur of its *own* state. That is the difference between this and lerp60, and it is
where the effort belongs.

## Open question, to answer before building

The draw-side perform lists must be verified side-effect-free when run twice. `performOnlyDraw`
shows the intent at the actor level, but the list-level passes (`Graffito`, `Pollution`,
`DrawBufGroup`) have not been checked, and a pass that consumes or steps a buffer would
double-step under a second draw. **Measure it; do not assume it** — the last four attributions in
this arc that were assumed rather than measured were all wrong.

## Probing double-run safety: the control fired before the result did

`SB_DOUBLE_DRAW` was added to answer the open question above — do the draw-side perform lists
mutate state when run twice?

* `=1` runs the DRAW lists twice (the question)
* `=2` runs the MOVEMENT list twice (**positive control** — movement is the physics step, so this
  MUST perturb the game; if it doesn't, the probe is blind)

Divergence was read from a state line printed every 200 ticks: Mario's position and status.

**The control failed, which is the useful result.** Baseline and double-movement produced byte
identical output:

    [dbl-draw] tick=200 pos=(6500.0000,300.0000,-3850.0000) status=0x133f
    [dbl-draw] tick=800 pos=(6500.0000,300.0000,-3850.0000) status=0x133f   <- same under =2

Mario is frozen in this scene — the known stuck WIN_DEMO start state (`mStatus=0x133f`, memory
`[[delfino-gameplay-renders-2026-07-17]]`). A motionless actor cannot diverge under doubled
physics, so **the probe cannot see divergence at all**, and a clean result from `=1` would have
meant nothing. This is exactly the failure the instruments rule exists to catch: without the
control, "double-draw shows no divergence" would have read as a green light to build on.

### What the run did establish

1. **`=1` crashes with the mapped-ByteBuffer overflow fatal** — drawing twice into ONE frame
   doubles the geometry and the per-frame vertex staging (3 MB) is sized for a single pass. This is
   NOT a blocker for interpolation, where each sub-frame is its own presented frame with its own
   packet; it is a blocker only for this probe's shortcut of doubling within one frame. (The
   overflow message named the offending draw, courtesy of the draw-desc rework earlier today.)
2. **The scene IS dynamic**, so a usable divergence signal exists: per-frame vertex counts
   oscillate over 81 distinct values (18074 / 18078 / 18082 / 18086 / 18090 …) as leaves sway.
   Mario being frozen does not mean nothing moves.

### Why per-frame vertex count is NOT the fix for the probe

Under `=1` the vertex count roughly doubles *by construction*. That direct effect swamps any state
perturbation, so comparing it against baseline cannot isolate a side effect. The signal has to be
read from **pass 0 only**, or be a non-geometry state readout of something that actually moves in
this scene. Picking that signal is the next step, and it must carry the same positive control:
`=2` has to diverge before `=1`'s answer is worth anything.

**Status: the double-run safety question is still OPEN.** It is not "probably fine".
