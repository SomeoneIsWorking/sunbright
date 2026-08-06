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

## ANSWERED: the draw pass is NOT idempotent, and the sub-frame boundary is PreEntry + draw

The cross-run probe above was the wrong shape. The right question is **idempotence within a single
tick**, which needs no dynamic scene and no baseline run: mark the GX fifo, run the draw block,
hash what it emitted, REWIND the fifo, run it again, hash, compare. Rewinding means the second pass
*replaces* the first, so total emitted geometry stays at 1x — no staging overflow, and the frame
still renders normally from the final pass. (`sb_gx_fifo_mark` / `_rewind` / `_hash` in
`extern/aurora/lib/gx/fifo.cpp`.)

The comparison carries a positive control and an empty-guard: `empty` counts ticks where either
pass emitted nothing, so "emitted nothing" can never be reported as "identical".

| mode | what runs between the two passes | result |
|---|---|---|
| `=2` **control** | `CalcAnim` (advances animation) | **1600/1600 DIFFERENT** — the comparison can see a change |
| `=1` | nothing | **1200/1200 DIFFERENT** — bytes 2,252,625 then 2,130,302 |
| `=3` | `PreEntry` | **DIFFERENT**, but bytes 2,252,625 then 2,252,789 |

**Reading it:**

1. **Re-running the draw lists alone does not reproduce the frame.** A consistent ~122 KB (5.4%)
   goes missing on the second pass.
2. **Re-running `PreEntry` first recovers essentially all of it** — the deficit closes from
   -122,323 bytes to +164. So the draw buffers are POPULATED by the entry pass and CONSUMED as
   they are drawn; a bare second draw finds them partly empty. The interpolation sub-frame
   boundary is therefore **`PreEntry` + draw**, not draw alone. The design assumed this; it is now
   measured rather than assumed.
3. **A +164 byte residual remains** on `=3` (0.007% of 2.25 MB). Small, but idempotence is a
   yes/no property and this is a no. Something still differs between two identical-input passes,
   and it must be identified before sub-frame rendering is built on top — a residual that
   accumulates per sub-frame would drift over a play session rather than showing up in a
   single-frame comparison.

That residual is the next question. Do not round it away.


## The residual is a DEBUG MARKER — WRONG, see the correction below

`=3`'s +164-byte residual was localised rather than guessed at: `sb_gx_fifo_snapshot` /
`sb_gx_fifo_compare` report the first differing offset, and `sb_gx_fifo_dump_heads` prints the
leading bytes of both passes.

    firstDiff = 0            <- they diverge at the FIRST byte, not at the end
    passA head: 50 00 22 00 0f 44 72 61 77 42 75 66 20 53 6b 79 20 4f 70 61   ("DrawBuf Sky Opa")
    passB head: 10 00 06 10 20 3f cb fa 20 00 00 00 00 40 09 3f 9a ...

`0x50` is `GX_AURORA`; subcommand `0x0022` is **`GX_AURORA_DEBUG_MARKER_INSERT`**. Pass A opens
with a debug marker carrying the draw-buffer name; pass B opens with `0x10` = `GX_LOAD_XF_REG`,
real matrix data.

So the head divergence is **diagnostic metadata, not render state** — our own draw-attribution
markers, emitted conditionally, landing differently on a re-run. The byte-stream comparison was
therefore answering a stricter question than interpolation asks: it compared *everything in the
stream*, including instrumentation, when what matters is whether the RENDER commands repeat.

This is the comparison-instrument rule again — *state what both sides actually measure*. The
instrument was sound at detecting a difference (its control fired), but the difference it found at
offset 0 is not the difference that matters.

### What survives this correction, and what does not

* **SURVIVES: `=1` is genuinely not re-runnable.** The ~122 KB deficit is three orders of magnitude
  larger than marker traffic and tracks real missing geometry — the draw buffers really are
  populated by entry and consumed as they are drawn. The sub-frame boundary really is
  `PreEntry + draw`.
* **DOES NOT SURVIVE: "`=3` leaves a +164 byte residual" as evidence of state mutation.** That
  number is contaminated by debug markers and cannot support a claim either way until markers are
  excluded from the comparison.

### Next

Compare with `GX_AURORA` debug sub-commands (`0x0020` push / `0x0021` pop / `0x0022` marker)
skipped, so the comparison covers render commands only. Keep the same positive control — it must
still fire with markers excluded, or the filtered comparison is blind.

Also noted: `=4` (three passes, comparing pass 1 against pass 2) never reaches its first report —
three draw passes plus two extra `PreEntry` runs stalls the frame. Not chased; the marker-filtered
two-pass comparison answers the same question more cheaply.


## CORRECTION: the residual is NOT the markers, and byte-identity was the wrong criterion all along

The section above concluded, from a single head dump showing a marker at offset 0, that the
+164-byte residual *was* the draw-attribution markers. **That is false.** Markers were then
suppressed in both passes (`sb_suppress_draw_markers`, gated at the one emitter,
`J3DDrawBuffer.cpp`), and:

* total stream size dropped ~2,147 bytes, so the suppression demonstrably worked;
* the positive control still fired (2600/2600 DIFFERENT), so the filter did not blind the probe;
* **the residual was unchanged — still exactly +164.**

Seeing a marker as the first differing byte and concluding it caused a size delta was a jump. The
first differing byte and the cause of a length difference are simply not the same thing, and one
observation was treated as if it explained both.

With markers gone, the heads read:

    passA: 61 88 00 fc 0f  61 fc ea 2d f5  61 80 00 01 90  61 84 00 00 00 ...
    passB: 10 00 06 10 20  3f cb fa 20 ...

`0x61` is `GX_LOAD_BP_REG`, `0x10` is `GX_LOAD_XF_REG`. Pass A emits BP register writes that pass B
does not, because pass A already set those registers to those values and the GX layer elides
redundant writes. **GX is a sticky state machine, so two passes from different entry states
legitimately produce different command streams.**

### The real error: chasing the wrong criterion for three iterations

Byte-identity was noted as "stricter than interpolation needs" and then pursued anyway. What
interpolation actually requires is:

1. the second pass **renders the same scene**, and
2. it **does not corrupt game state**.

Neither is a byte-equality property, and no amount of refining the byte comparison will answer
them. The comparison should have been abandoned at the point stickiness was identified.

### What stands

* **`=1` is not re-runnable**: -122,323 bytes, unchanged with markers suppressed. Draw buffers are
  populated by entry and consumed as drawn. The sub-frame boundary is `PreEntry + draw`. This is
  three orders of magnitude past marker or state-elision traffic and is the finding worth keeping.
* **`=3` reproduces the frame to within 164 bytes of 2.39 MB (0.007%)**, with GX state elision a
  sufficient explanation. Good enough to proceed.

### Next, and it is not another byte comparison

Render two sub-frames and compare the **images**. With the same interpolation alpha the two
presented frames must be pixel-identical; with different alphas they must differ in the way
interpolation predicts. That tests what actually matters, and the existing `SB_DUMP_FRAME` harness
already produces the artefact to compare.

## ANSWERED, on the criterion that matters: a re-run pass reproduces the frame PIXEL-IDENTICALLY

The byte comparison was abandoned for the right test: **compare the images**.

No new machinery was needed. `SB_DOUBLE_DRAW=3` already rewinds pass 0 and lets pass 1 render, so
the frame it presents *is* a sub-frame's output — `PreEntry` + draw, run from post-draw state. Dump
it and compare against a normal frame at the same checkpoint.

    baseline (normal single pass) vs mode 3 (rendered from a RE-RUN pass)
      pixels: 1228800   differing: 0   (0.0000%)
      PIXEL-IDENTICAL

**With a positive control**, because a comparison reporting "identical" is indistinguishable from a
broken one. Mode 1 (draw-only re-run, the variant known to lose ~122 KB of geometry) was compared
the same way:

    control: mode 1 vs baseline
      differing pixels: 1956 of 1228800  (0.16%)  -> the comparison CAN detect a difference

So:

* **A sub-frame pass (`PreEntry` + draw) renders the frame exactly.** The +164-byte stream
  difference is render-neutral, exactly as the GX-state-elision explanation predicted — and this is
  the evidence for that, rather than the reasoning being taken on trust.
* **Dropping `PreEntry` is visibly wrong**, not merely different: 1,956 pixels change. The
  sub-frame boundary is confirmed as `PreEntry + draw` by image, not just by byte count.

Note the control's own lesson: mode 1 loses 122 KB of geometry yet moves only 0.16% of pixels —
mostly occluded or small. A whole-frame **mean** would have hidden that entirely (it is why mean
RGB stayed at 135.2/144.6/145.8 through both). Pixel-exact comparison was necessary; the mean is
not a substitute.

### Where this leaves the design

The sub-frame render path is **verified sound**. What remains is the part that was always the real
work, and it is untouched by any of the above:

1. Snapshot actor transforms before `moveObject`, interpolate them per sub-frame.
2. Present twice per tick (the seam is `sb_frame_present`, currently once per `gameLoop`).
3. **Effects that are not a function of an actor transform** — the ghost/dash trail, JPA particles,
   anything rebuilding vertex data per tick — need prev/cur of their own state. This is the class
   that jittered under lerp60 and the reason "game native" is the requirement.

## Implementation started, and the first hook is in the WRONG PLACE

`TLiveActor` now carries `mSbPrevPosition` / `mSbPrevRotation` / `mSbPrevValid` (native-only,
appended, so the guest-offset comments stay correct), snapshotted immediately before `moveObject()`
in `TLiveActor::perform`. Verified render-inert: 0 of 1,228,800 pixels differ from baseline.

A snapshot that is inert at render time is indistinguishable from a snapshot that is **useless**,
so a motion probe was added — how many actors actually move per tick:

    [interp] snapshots=2340000 moved=0 (0.0%) maxStep=0.000 units

**Zero, over 2.34 million snapshots.** Taken at face value that says transform interpolation would
be a no-op, and building it would have produced no visible change while looking like it worked.

### It is a POPULATION error, not a static scene

`TMario::perform` is a **full override that never chains to `TLiveActor::perform`**, and
`grep -c '::perform(u32'` finds **231 classes overriding it**. So the hook only sees actors that
use the base implementation — a minority that excludes Mario and most things that move. `moved=0`
describes that subset, not the scene, and the scene is demonstrably dynamic (vertex counts
oscillate over 81 values).

This is the same failure as the earlier Mario-position probe: a measurement whose *population*
silently excludes the interesting cases. The probe now states its own blind spot in every line it
prints, so the number cannot be read as more than it is:

    | COVERS ONLY actors using TLiveActor::perform; 231 classes override it (incl. TMario)
      and are NOT counted here

### Consequence for the design

The snapshot cannot live in `TLiveActor::perform` — 231 overrides bypass it. It has to sit
somewhere every actor passes through regardless of what it overrides. The right shape is a
**virtual on the actor base** (`TPlacement` owns `mPosition`, `TActor` owns `mRotation`), default
no-op, overridden once to snapshot, and invoked from the perform-list dispatch
(`TPerformList::perform` -> `forEachPerform`) on the movement phase. That reaches every actor
without touching 231 classes.

The fields and the inertness verification stand; only the call site is wrong.

**Do not read the current `moved=0` as evidence about the scene.** It is evidence about a hook
that needs moving.

## Reaching every actor is harder than expected — the object graph is heterogeneous

The snapshot hook was moved off `TLiveActor::perform` (bypassed by 231 overrides) onto a virtual
`TViewObj::sbSnapshotInterp()`, default no-op, implemented on `TActor` (which owns `mRotation` and
inherits `mPosition`, so it covers map objects too, not only live actors). The fields moved to
`TActor` with it.

Driving that virtual to every actor has taken **three attempts, each of which silently visited
nothing**:

1. **Gated inside `forEachPerform` on `(mask & link->unk8) & 1`.** Never fired — the movement
   list's dispatch mask is not that simple. Guessing at phase-bit semantics produced a hook that
   did nothing while compiling and running fine. Replaced with an explicit
   `TPerformList::snapshotInterp()` called from `TMarDirector::direct()`, because an explicit call
   cannot silently mismatch.
2. **Walking the list's direct children.** The perform lists are a **tree** — a child can itself be
   a `TPerformList`, whose default no-op stopped the walk. Fixed by overriding
   `sbSnapshotInterp()` on `TPerformList` to recurse.
3. **Managers.** `TObjManager` is a `TViewObj` (so the walk reaches it) but owns its actors in
   `unk18[0..mObjNum)`, not as perform-list children — so it too stopped the walk. Given a
   forwarding override.

After all three, the pass reports `snapshotInterp visited 18 direct children` — it demonstrably
runs — but the actor-level motion probe **still never fires**, so actors reaching
`TLiveActor::perform` are still not being snapshotted. There is at least one more container type in
the path.

Everything above is verified **render-inert**: 0 of 1,228,800 pixels differ from baseline.

### Why this is being written down rather than pushed further

Each hop was found the same way — by a counter that reported a suspiciously round number (`moved=0`
over 2.34M samples, then `visited 18`) rather than by reading the code. That is working, but
chasing a heterogeneous object graph hop by hop is expensive and there is no bound on how many
container types remain.

**The alternative worth trying first next time: don't enumerate at all.** What interpolation needs
is "the transform the previous frame displayed". Every actor that RENDERS necessarily runs its own
calc/draw path, so each can record its own transform there — capturing `prev` exactly for the set
that matters, with no global walk and no dependency on the container hierarchy. The enumeration
approach requires knowing every container type; the capture-at-render approach requires none.

Cost of the detour so far: the virtual, the `TActor` fields, and three container overrides — all
inert, all keepable if enumeration is ever wanted, none of it yet delivering a snapshot.

## Enumeration abandoned after five hops — and a process failure worth recording

Continuing the walk added two more hops and still reaches no actor:

4. **Group containers.** The movement list's 18 children were finally NAMED rather than guessed at
   (they are Shift-JIS; decode before reading): マップグループ, 波, 落書きグループ, コンダクター,
   マネージャーグループ, プレーヤーグループ, camera 1, 水マネージャ, 空グループ, … A recursion
   counter then showed `nested-list recursions so far=0` — **none of the 18 is a `TPerformList`**,
   so the walk stopped dead at the first level the entire time.
5. **`TViewObjPtrListT`.** That is what the groups actually are (`TDStageGroup :
   TViewObjPtrListT<TViewObj>`). Given a forwarding override — and the actor probe *still* never
   fires, and `TObjManager`'s forwarder still never fires.

Five container hops, every one verified to run, none reaching an actor. All of it render-inert
(0 of 1,228,800 pixels differ).

### The process failure

The previous entry closed with: *"The alternative to try first next time: don't enumerate at all."*
Then this session spent its entire iteration enumerating. Recognising the wrong approach and then
continuing it anyway is worse than not recognising it — the decision had already been made on
evidence and was silently discarded in favour of one-more-hop momentum.

Each hop was cheap and each felt like the last one, which is exactly what makes this shape of chase
expensive. The counters were doing their job throughout (`moved=0`, `visited 18`, `recursions=0`,
forwarder never firing); the failure was in what was done with them.

### Decision, recorded so it is not silently discarded again

**Stop enumerating. The next iteration implements capture-at-render and nothing else.**

Rationale, unchanged from before: enumeration requires knowing *every* container type in a
heterogeneous graph, and five are known not to be sufficient. Capture-at-render requires none —
every actor that renders necessarily executes its own calc/draw path, so the transform can be
recorded there, for exactly the set of objects that matter.

The inert plumbing (the `TViewObj` virtual, `TActor` fields, and four forwarding overrides) is left
in place: it is harmless, and it is the correct machinery *if* an enumeration is ever wanted for a
different purpose. It is not deleted, but nothing should be built on it.

## SOLVED: the snapshot reaches every actor — hook the DISPATCH FUNNEL, not the object graph

Capture-at-render was the recorded decision, and the shape it actually took is better than the one
that was written down: rather than each actor recording its own transform at draw time, the
snapshot is driven from **`JDrama::TViewObj::testPerform`**.

That function is **non-virtual**, and it is the single point every container dispatches through:

| container | dispatch |
|---|---|
| `TPerformList::forEachPerform` | `it->unk4->testPerform(...)` |
| `TViewObjPtrListT::perform` | `it->testPerform(...)` |
| `TStrategy::perform` | `unk10[i]->testPerform(...)` |
| `TObjManager` / `enemymanager` | `getObj(i)->testPerform(...)` |
| `TViewConnecter`, `TScreen`, `TDirector` | `unk10/unk14->testPerform(...)` |

`TStrategy` is a **sixth** container type, and none of the five enumeration hops would have found
it — confirming the walk had no bound. Hooking the funnel needs no knowledge of container types at
all: an object that is performed passes through here *by construction*, and being non-virtual,
nothing can override its way around it.

The snapshot fires on the surviving `CUE_MOVE` bit, so it lands immediately before that object's
movement — exactly where `(prev, cur)` is well defined. A **tick guard** (`mSbPrevTick` vs
`TViewObj::sSbInterpTick`, opened once per tick by `TMarDirector::direct`) stops an object
dispatched twice in a tick from overwriting `prev` with `cur` — which would silently flatten
interpolation to a no-op *and render exactly like a correct implementation*.

### Coverage is demonstrated, not asserted

`moved=0` has two completely different causes — a static scene, and a hook that reaches nothing —
and a counter alone prints `0` for both. The probe therefore also records **who it saw**:

    ROSTER: 329 distinct objects snapshotted in the first 200 ticks
      ... マリオ ... マリオエフェクト ... ヨッシーの卵 ... 水ヒットコイン ...

**`マリオ` is in the roster.** `TMario::perform` is the canonical full override that never chains
to `TLiveActor::perform`, and it is the object every previous attempt failed to reach. Coverage is
now a measured fact.

(The first roster run capped at 192 entries and said so in its own output — `(CAPPED -- the real
count is higher)` — instead of presenting a truncated list as complete. Cap raised to 2048; the
real count is 329.)

### What the numbers then mean

    SNAPSHOT pop: samples=5580000 moved=1 (0.0%) maxStep=2200.000 by "陽炎"

With coverage established, this reads as a statement about the **scene**: in the Delfino start
state no actor transform changes. Mario is in the frozen WIN_DEMO start state
(`mStatus=0x133f`, memory `[[delfino-gameplay-renders-2026-07-17]]`), and the plaza's visible
motion — swaying palm leaves — is *joint animation*, not `mPosition`. The single mover is 陽炎
(heat-haze) making one 2200-unit init teleport.

So transform interpolation has nothing to show in this scene, and **this scene cannot validate it**.
Validating it needs actors that actually move; that is the next step, not more hook work.

### Also fixed here: a dangling `else` the previous hook introduced

The snapshot used to be called from `TMarDirector::direct()` like this:

    if (unk4E & 1)
        mShinePfLstMov->perform(...);
    else
    #ifdef SMS_NATIVE_PLATFORM
        mPerformListMovement->snapshotInterp();
    #endif
        mPerformListMovement->perform(...);

Under `SMS_NATIVE_PLATFORM` the `else` bound to the hook **alone**, making
`mPerformListMovement->perform()` unconditional — so on shine stages *both* movement lists ran.
That is precisely the both-branches-drive-the-same-list misdecompilation the comment four lines
above it warns about, reintroduced by an `#ifdef` without braces. Now braced.

### Removed

The five enumeration overrides (`TPerformList::snapshotInterp` + its `sbSnapshotInterp`,
`TViewObjPtrListT::sbSnapshotInterp`, `TObjManager::sbSnapshotInterp`, `sSbRecurseCount`) are
**deleted**, not left inert. Leaving them would have been actively wrong rather than merely
harmless: they are recursive, and the funnel hook now invokes `sbSnapshotInterp()` on containers
too, so each would have re-walked its whole subtree on top of the per-object dispatch — two
mechanisms with different coverage feeding one counter.

## The 60fps arc is now blocked on a GAMEPLAY defect, not on interpolation machinery

With coverage proven, the probe answers a question it could not answer before, and the answer
redirects the arc.

Driving real input (`SB_PAD_SCRIPT="400:UP 1400:UP+B 2400:UP"`) changes nothing:

    SNAPSHOT pop: samples=12800000 moved=1 (0.0%) maxStep=2200.000 by "陽炎"

`マリオ` is in the snapshot roster and the total number of moving objects across 12.8M samples is
**one** (the 陽炎 heat-haze init teleport). So Mario's `mPosition` is *literally constant* — this is
measured, not inferred from "Mario looks stuck".

    [mario] perform(0x200): unk114=0x410 VISIBLE=0 UNK4=0 -> doEntry=0 mStatus=0x133f
            pos=(6500,300,-3850)

That is the known frozen WIN_DEMO start state (memory `[[delfino-gameplay-renders-2026-07-17]]`),
and it does not respond to pad input.

**Consequence for the arc, and it is the useful finding here:** every remaining step of game-native
interpolation — writing `lerp(prev, cur, alpha)` into the live fields, calling `performOnlyDraw`,
presenting twice per tick — is unverifiable while no actor moves. A sub-frame at alpha=0.5 would be
pixel-identical to alpha=1.0 for the entire scene, so the implementation would pass every check it
could be given *while doing nothing*, which is precisely the failure mode this whole entry has been
guarding against.

So the next step is **not** more interpolation machinery. It is getting Mario out of `mStatus=0x133f`
into normal gameplay, which the port needs regardless. Interpolation resumes once the probe reports
a non-trivial `moved%` — and that reading is now trustworthy, because coverage was demonstrated
rather than assumed.

## CORRECTION: the dash/ghost trail is NOT geometry, and the RE for it is already banked

This entry has said twice that the ghost/dash trail is *"geometry rebuilt each tick from discrete
30 Hz position samples"*, and used it as the headline example of the class needing prev/cur of its
own state. **That is false.**

`TAfterEffect : public JDrama::TViewObj` (`include/MarioUtil/ScreenUtil.hpp`) holds a
`JUTTexture* unk10` — an **EFB copy of the frame** — and `perform` draws an 8-vertex
`GX_TRIANGLEFAN` over the viewport textured with it, sampled slightly scaled and offset, alpha
blended over the new frame. It is a **screen-space temporal feedback blur**, not per-actor
geometry. There is no per-tick vertex rebuild and no position-sample history to interpolate.

Its 60fps failure mode is correspondingly different, and it was diagnosed and FIXED in the recomp
already (`sms-recomp/overrides/afterimage.cpp`): interpolation presents twice per tick, both
emissions replay the recorded pass list *including its EFB copies*, so the screen texture is
written twice per tick from two different images (the interpolated pose at t-0.5 and the true pose
at t) and the feedback advances at double rate. The fix is not prev/cur state — it is telling the
host **which EFB copy is a next-frame feedback texture** rather than an intra-frame copy, and
advancing that one once per tick. Nothing structural distinguishes the two; the authority is the
effect itself — whatever texture `TAfterEffect` samples IS the feedback texture.

Note what that file also gets right and this entry had not stated: the smoothing constants are
deliberately NOT rescaled, because game logic still ticks at 30 Hz under render interpolation. Only
the capture rate was ever wrong.

**Where this leaves the effects taxonomy** — the residual class is smaller than claimed:

* dash/ghost trail — **screen-space EFB feedback**; solved in recomp, needs no prev/cur;
* JPA particles — still a genuine independent simulation needing its own prev/cur;
* anything genuinely rebuilding vertex data per tick — still owed, but the trail was the only named
  member and it turned out not to be one.

## The decomp is not an alternative runtime here — it is where the RE lives

A recomp override cannot be written without guest addresses, field offsets and phase semantics, and
those are exactly what the decomp records. The existing overrides already work this way and say so
in their own headers: `afterimage.cpp` opens with *"THE EFFECT (RE: decomp/sms/src/MarioUtil/
ScreenUtil.cpp, TAfterEffect)"*. So the split is not decomp-vs-recomp, it is:

* **decomp** — RE the mechanism, with named fields and a compiler checking you; prove the seams.
* **recomp** — run the validated design against the whole game, where the actors actually move.

### RE inventory for game-native interpolation: BANKED vs OWED

Banked (usable by an override today, no new RE):

| item | value | source |
|---|---|---|
| `TPlacement::mPosition` | guest `0x10` | `JDRPlacement.hpp` |
| `TActor::mScaling` / `mRotation` | guest `0x24` / `0x30` | `JDRActor.hpp` |
| `TViewObj::testPerform` | US `0x802fcc94`, JP `0x80046F6C`, size `0x68` | funcs list / `symbols.txt` |
| it is THE dispatch funnel, non-virtual | measured this session (6 container types) | this entry |
| MOVE cue is bit `0x1` | `CUE_MOVE`, `JDRViewObj.hpp` | upstream naming |
| sub-frame boundary = `PreEntry` + draw | measured: re-run pass is **pixel-identical**, and dropping PreEntry moves 1,956 px | this entry |
| `TAfterEffect::unk10` = feedback texture | guest `0x10` | `ScreenUtil.hpp` / `afterimage.cpp` |

Owed (genuine RE still to do):

* JPA particle emitter state — what constitutes prev/cur for an independent simulation;
* whether any other effect rebuilds vertices per tick (the trail was the only named candidate and
  it is not one, so this may be empty — but "may be empty" is a hypothesis, not a finding);
* the render-cost gate: two draw passes per tick against a 16.6 ms budget. C019's ~15.8 ms is
  **decomp-measured**; the recomp figure must be measured, not carried over.

The transform half of the RE is therefore complete. That is what today's decomp work bought, and it
transfers to a recomp override unchanged.

## The recomp 60fps stack already exists — and the fork is COST vs COVERAGE, not feasibility

`sms-recomp/runtime/lerp60.h` + `overrides/native_frame.cpp` are a mature stack: two presents per
30 Hz tick, **mid-tick pacing** between them (sixty presents a second is not sixty frames to the eye
with vsync off), camera-cut detection taken from the game's own
`CPolarSubCamera::warpPosAndAt` rather than inferred from eye movement, tag-coverage reporting that
carries its denominator, and `SBR_SMOOTH` — a validator that distinguishes genuine interpolation
from the same picture presented twice, per screen cell, **validated against both classes**.

Its founding premise is the one this arc rejected:

> *a tick contributes MATRICES, not geometry — model-space vertices do not change between ticks...
> not two scene submissions.*

That premise is false for a known, already-RE'd class: `TMapObjWave`'s immediate-mode ripple grid
and `calcDrawVtx` splash geometry are rebuilt per tick, and JPA particles are an independent
simulation. Those are precisely what has no matrix to lerp.

### What game-native reuses, and what it replaces

**Reuses (all of it runtime-agnostic and already built):** the two-present frame seam, mid-tick
pacing, camera-cut declaration, afterimage feedback-texture identification, `SBR_SMOOTH`, and the
`SBR_LERP60` default-off gating that keeps aurora's oracle role uncorrupted.

**Replaces:** the core. Instead of patching lerped matrices into a retained uniform block, write
lerped transforms into the **guest** fields and re-run `PreEntry` + the draw lists, so the game
regenerates everything at alpha — including geometry that has no matrix.

### Premise 2 does NOT block this, and the cost picture has moved

`2026-07-30_aurora_60fps_lerp_design.md` "Premise 2: re-executing a recorded frame — DESTROYED"
is about replaying **aurora's own recorded frame** (staging unmapped in `end_frame`, `Range`s into
globally shared buffers overwritten from offset 0, and a FIFO parse that is not idempotent). Every
one of those is a statement about the *record/replay layer*.

Game-native re-runs the **game's** draw lists, which regenerates the recording from scratch. That is
a different layer, and it is the layer this session measured: a `PreEntry` + draw re-run reproduces
the frame **pixel-identically** (0 of 1,228,800 differ), with dropping `PreEntry` as the positive
control at 1,956 px. So the safety question game-native actually depends on is answered.

What it costs is the open question, and **the answer moved this week**. lerp60's cheap shape existed
because a second submission meant re-uploading geometry; the persistent geometry arena (`bcbb418`)
has since driven per-frame array uploads to **zero bytes in the steady state**. The dominant cost of
a second submission at design time is now largely gone. That does not make it free — the FIFO parse
and draw-call issue remain — but the 2026-07-30 cost reasoning must be re-measured rather than
inherited.

### So the fork is:

| | matrix-lerp (built) | game-native (directed) |
|---|---|---|
| coverage | only what is a model matrix; everything else bespoke | everything the game regenerates, by construction |
| cost | ~1x geometry + a uniform-only second pass | second full scene submission |
| safety | proven in recomp | re-run proven pixel-identical (decomp, this session) |
| trail | needed its own EFB fix | same EFB fix still needed (it is a capture-rate bug either way) |

**Next measurement, and it decides the arc:** recomp frame cost on Delfino, and the marginal cost of
a second `PreEntry`+draw with the geometry arena in place. C019's ~15.8 ms is decomp-measured and
pre-arena; carrying it over would be exactly the inherited-number error this file keeps catching.

## MEASURED: Mario moves in the recomp, and the render-cost gate is the binding constraint

Two measurements, both of which were assertions in this file until now.

### 1. Mario MOVES in the recomp — the frozen Mario is a DECOMP gap

The earlier recommendation rested on "in recomp Mario moves", which had **not** been measured. It is
now. Driving the analog stick (`SBR_PAD_SCRIPT="400:STICK=0/100,1200:STICK=90/0,2000:STICK=0/100+A"`,
`SBR_LUCENT_DEBUG=mario`):

    [mario] pos (6500.0, 300.0, -3850.0)
    [mario] pos (6500.0, 300.0, -3306.6)
    ...
    [mario] pos (7420.9, 100.0,  -200.9)      <- Y steps 300 -> 100, he walks down a level
    -> 83 distinct positions

Note the first control run gave **no** input and Mario sat at exactly the decomp's
`(6500, 300, -3850)`. That looked like confirmation of a frozen Mario and proves nothing — a
motionless player under no input is the expected result. Only the stick-driven run is evidence, and
buttons alone would not have done it (`native_pad.cpp` says so explicitly: Mario moves on the analog
stick).

So the decomp's `mStatus=0x133f` is confirmed a **decomp porting gap** — the stage-entry chain that
would call `rollingStart`/`returnStart` has no caller in the ported source — and not a property of
the game. Interpolation is verifiable in the recomp and is not verifiable in the decomp today.

### 2. The render-cost gate is real, and it is the binding constraint

`SBR_PRESENT_TIMING=1` with pacing off (`SB_TURBO=1`), Delfino Plaza, one present per tick:

    [ptime] present gaps: alternating means 23.93 ms / 23.98 ms

**~24 ms per present.** `SBR_J3D_CAPTURE` defaults OFF and was unset, so this is not capture
overhead — it is close to the real cost, with a residue of always-run diagnostic mirrors
(`GXSetBlendMode`/`GXSetZMode`/`GXLoadTexObj`).

For 60 fps, each present must complete in **≤16.7 ms**. One pass already costs 24 ms. A game-native
sub-frame is a *second full scene submission*, so a tick lands near 48 ms — about 20 fps effective,
i.e. **worse than the 30 fps it started from**.

Two honest caveats: the running means were still drifting down (24.43 -> 23.93) and had not
converged, and this must NOT be compared against C019's decomp ~15.8 ms — different runtime,
different instrument, different sample count, and pre-arena. It is a recomp number, standing alone.

### What this does to the fork

It does not make game-native wrong; it makes it **unaffordable at today's render cost**, which is a
different problem with a different owner. The ordering that follows:

1. **Render cost first.** 24 ms -> under ~8 ms is the prerequisite for ANY two-pass scheme. Until
   that happens, a correct game-native implementation would ship a game that runs slower than the
   one it replaces, and the user's directive was explicitly *"if you pick the good path and
   implement it then it'll already be fast"* — 48 ms/tick is not that.
2. Only then the game-native core, against the RE inventory already banked above.

The matrix-lerp stack's cheap shape looks better under this constraint precisely because it avoids
the second submission — which is the trade the 2026-07-30 design was making deliberately. This
entry's job is to say that the trade is a COST trade, not a correctness one, and that it should be
re-decided after the render cost is known rather than inherited from either direction.

## PROFILED: the 24 ms has no single culprit — and my "~48 ms" estimate was WRONG

### The instrument

`perf` is not installed here. Sampled instead with `eu-stack` (poor-man's profiler,
`$CLAUDE_JOB_DIR/tmp/pmp.sh`), which reports `attempted / ok / failed` and **refuses to print a
profile when zero samples succeeded** — "no hot spots" and "never sampled" must not look alike.
Blocked worker threads (`__syscall_cancel_arch`, futex/epoll waits) are excluded; only game-thread
leaf frames are counted.

**Sample count mattered.** At N≈87 `override_lookup` looked like the top cost at 13%. At N=329 it is
**2.4%** — the first reading was noise, and acting on it would have optimised the wrong function.

### Game-thread leaf profile, N=329

| cluster | samples | share | what |
|---|---|---|---|
| guest memory accessors — `sb_r32` 28, `sb_w32` 9, `psq_load` 4 | 41 | 12.5% | every guest load/store |
| aurora GX — `draw_prim` 24, `prepare_idx_buffer` 9, `XXH3` 10 | 43 | 13.1% | GX stream -> draw records |
| guest call dispatch — `call_ppc` 31, `override_lookup` 8 | 39 | 11.9% | two table lookups per call |
| GX FIFO pipe — `parse` 18, `fifo_write` 13, `vertex_size` 6 | 37 | 11.2% | `dev_gxfifo` write-gather |
| tail — `mmio_write` 6, `memcpy` 7, `strncmp` 3, guest funcs | ~40 | ~12% | |

Four clusters of roughly equal size. **There is no single 24 ms culprit**, so no single fix gets a
3x speedup — eliminating any one cluster entirely leaves ~21 ms.

Two concrete inefficiencies are nameable from this, though neither is a silver bullet:

* `fifo_write` does `g_buf.insert()` per 1-4 byte store and then
  `g_buf.erase(g_buf.begin(), g_buf.begin() + used)` — an O(n) memmove of the remainder on every
  parse batch. A ring buffer removes it outright.
* `vertex_size` appearing as a leaf at all suggests it is recomputed per vertex rather than cached
  per vertex-format change.

Also corrected: an earlier line here reasoned "aurora's `end_frame` is 40 μs, so rendering is not the
cost". `end_frame` genuinely is 40 μs (`SB_PROFILE_GFX`: drain=0, finish=1, submit/record=39 μs), but
aurora's GX work is **13.1%** of the game thread — it happens during guest execution as the FIFO is
written, not inside `end_frame`. Profiling one function and generalising to a subsystem was the error.

### The estimate that was wrong, and it changes the conclusion

I told the user a game-native sub-frame lands "near 48 ms/tick — worse than the 30fps it replaces".
That assumed the second pass costs a **whole extra tick**. It does not. A sub-frame re-runs
`PreEntry` + the draw lists — it does **not** re-run movement, physics, AI or collision.

So the marginal cost is the draw-side clusters (aurora GX 13.1% + FIFO pipe 11.2% + the guest draw
code), not 100% of the tick. Order of magnitude: **+8-12 ms**, giving ~32-36 ms per tick.

The budget for interpolated 60fps is **33.3 ms per 30 Hz tick** (logic once, draw twice, present
twice). That is not "hopeless" — it is **borderline**, needing roughly a 10-20% whole-frame
speedup rather than the 3x my wrong estimate implied.

That materially changes the fork. Game-native is not priced out; it is close enough that the two
named inefficiencies above plus ordinary tuning could cover it. The next measurement is the one that
settles it: **split the tick into logic-side and draw-side** so the marginal cost of a second draw
pass is measured rather than estimated from a leaf profile. Until that exists, ±10 ms of this is
arithmetic on sample shares, and this file has been burned by exactly that before.

## INSTRUMENT DEFECT: every frame-cost number above was noise — and the corrected baseline is 14.9 ms

### How it was caught

Building the tick-split instrument produced an A/B that looked like heavy perturbation: override
disabled ~28.5 ms/present, enabled ~48 ms. But the re-targeted version ran only **15 calls per
tick**, which cannot cost 20 ms. Rather than accept the explanation, the missing control was run —
**A against A**, same binary, same pad script:

    ARM A run 1: 25.65 ms / 26.00 ms
    ARM A run 2: 18.32 ms / 18.41 ms      <- 38% apart, identical configuration

So the spread was never the instrument. **`SBR_PRESENT_TIMING` reports a running mean over every
present since start**, so a faster run has reached a different frame — and a different part of the
scene — by the time any given line prints. Comparing two running means across runs is comparing
different populations. This is the project's own documented rule (*"never compare aggregates taken
at different sample counts; use the `COMPARABLE @ N=` line"*) and the recomp's instrument simply had
no such line, so every A/B in this file's preceding sections was invalid:

* "~24 ms per present" — a running mean, and additionally taken with **Mario stationary** while
  every later run had him walking the plaza. Two confounds at once.
* "the instrument perturbs frame time 2x" — unsupported; within run-to-run spread.
* the 32-36 ms sub-frame arithmetic built on top of 24 ms — void, because its input was void.

### The fix

`SBR_PRESENT_TIMING` now also emits a **fixed-window** mean (`SBR_PTIME_LO`/`HI`, default presents
600..1200), so two runs are compared at the same N over the same stretch of scene. It prints
`NO SAMPLES` explicitly when a run never reaches the window — a run that died early must not be
readable as a fast frame time.

### Second defect: the first run after a rebuild is cold

With the fixed window, three identical runs gave:

    A1: 31.11 ms      A2: 15.06 ms      A3: 14.83 ms   (N=600 each)

A2 and A3 agree to **1.5%**. A1 is double, and it is the first run after a rebuild — 462 pipelines
compile into a cold Dawn cache. **Measurement protocol: after any rebuild, discard the first run.**
Neither the fixed window nor a repeat run alone would have caught this; it took three.

### The corrected baseline, and what it does to the arc

**~14.9 ms per present**, Delfino, Mario walking, one present per tick, repeatable to ~1.5%.

The budget for interpolated 60fps is 33.3 ms per 30 Hz tick (logic once, draw twice, present twice).
Against 14.9 ms that leaves roughly **18 ms of headroom** for the second `PreEntry`+draw — and the
draw side is only a fraction of the 14.9 ms, since that figure includes movement, physics, AI and
collision, which a sub-frame does not re-run.

So game-native interpolation looks **affordable**, not borderline and not priced out. That is the
third different answer this file has given to the same question, and the difference is that this one
rests on a repeatable measurement with a validated comparison window rather than on a single running
mean. It should still be confirmed by measuring the sub-frame's marginal cost directly — but the
gate that looked like it might block the whole approach does not.

## The harness's RESOLUTION LIMIT — and why the affordability answer survives it anyway

The fixed window fixed the *population* problem but not the *noise* problem. Block-ordered A/B
(all OFF, then all ON) produced an impossible result — the instrument appearing to make the game
**faster**:

    OFF: 18.10, 17.86      ON: 14.71, 14.91

Each pair agrees internally to ~1.4%, but the blocks differ by 20%, and OFF ran first. **Consecutive
runs cluster**, so block ordering confounds the treatment with drift over minutes (host CPU
frequency / contention — the game state at presents 600..1200 is identical every run, because the
pad script keys on frame count).

Interleaving OFF/ON/OFF/ON/OFF/ON:

    pair1  OFF 20.78  ON 14.77     pair2  OFF 14.94  ON 15.73     pair3  OFF 16.07  ON 15.92

The sign of the difference **flips between pairs**. So the tick-split instrument's overhead is below
the noise floor — and more usefully: **this harness cannot resolve differences smaller than about
20%.** It must not be used to A/B a micro-optimisation; it would report whichever arm happened to
run in a quiet minute.

### Why the conclusion still holds

The affordability question does not need 20% resolution. Baseline is ~15-16 ms per present against a
**33.3 ms** tick budget. Even if the second `PreEntry`+draw cost as much as the *entire* current
tick — a deliberate over-estimate, since a sub-frame does not re-run movement, physics, AI or
collision — the tick lands near 30 ms, still inside budget. The margin is more than twice the noise,
so the answer is robust to exactly the uncertainty this section documents.

**Game-native interpolation is affordable.** Further precision on the sub-frame's marginal cost is
not required to justify building it, and pursuing it here would be measuring for its own sake — the
instrument is at its limit and the decision it was built to inform is already determined.

Stop measuring; implement.

## The RE inventory was INCOMPLETE: a recomp override also needs TYPE IDENTITY, and that is not banked

The "banked vs owed" table above listed `mPosition` @ `0x10`, `mRotation` @ `0x30` and
`testPerform` @ US `0x802fcc94` as sufficient for the override. **It is not**, and the omission is
the dangerous kind — everything listed is correct, so the table reads as complete.

`testPerform`'s `this` is a `JDrama::TViewObj*`. `mPosition` lives on `TPlacement`, several levels
down. **Not every `TViewObj` is a `TActor`** — perform lists, screens, view connecters, effect
objects and 2D screens are all `TViewObj`s that are not actors. In the decomp this is a non-issue:
`sbSnapshotInterp` is a virtual, so the compiler guarantees `mPosition` exists on anything that
reaches the body. Across the guest boundary there is no type — only an address — and writing a
lerped transform to `guest + 0x10` of a non-actor **corrupts whatever that object keeps at 0x10**.

That is a memory-corruption bug that would appear as an unrelated subsystem misbehaving, i.e. the
worst class of defect this project has.

### The principled discriminator, and why it is not available yet

The sound test is the guest **vtable pointer** at `+0x00`: an object is a `TActor` iff its vptr is
one of the vtables of `TActor` or a class derived from it. That set is RE, and RE lives in the
decomp — the class hierarchy is right there in the headers.

The blocker is addresses. `reference/` holds exactly two files:

* `sms_gmse01_funcs.txt` — **US**, functions only, **zero** `__vt__` symbols;
* `sms_gmsj01_symbols.txt` / `decomp/sms/config/GMSJ01/symbols.txt` — **JP**, 1,508 vtables.

The recomp runs the **US** build (every override address in it is US). So the vtable set exists in
the JP symbol data and the class hierarchy exists in the decomp, but **no current reference file
gives US vtable addresses**.

### The approach for next iteration

A vtable is an array of pointers into `.text`, and the US function addresses ARE known. So US
vtables are recoverable by scanning the US DOL's data sections for runs of pointers that all land on
known US function entry points, then naming each candidate by the methods it contains (a vtable
holding `JDrama::TActor::getType` is `TActor`'s; a class is TActor-derived if it carries TActor's
un-overridden slots). The decomp hierarchy gives the expected slot layout to match against.

That tool needs a coverage report as a first-class output, not an afterthought: a class whose vtable
is not recovered is an actor that silently never interpolates, and — unlike the decomp's virtual —
nothing will complain. "Recovered N of M TActor-derived classes, and here are the M-N that were
not" is the only form in which its result is usable.

**Until that exists, the recomp override cannot be written safely.** The decomp-side snapshot
(committed, coverage-proven, render-inert) stands as the RE that the override will consume.

## US vtable recovery: `tools/re/us_vtables.py` — 96.2% coverage, and the bug that looked right

The recomp override needs a runtime "is this guest object a TActor?" test, and the sound one is the
vtable pointer at `+0x00` against the set of TActor-derived vtables. US vtable addresses exist in no
reference file (`sms_gmse01_funcs.txt` is US but functions-only; the 1,508 vtable symbols are JP), so
they have to be recovered from the US DOL.

### The first version was wrong in the way that matters

It scanned for runs of words that are **known US function entries**. That produced 2,007 candidates,
379 TActor-tagged, and 56.1% coverage — a result that reads as "works, somewhat incomplete".

It was unusable. Many virtuals are absent from the US list (weak/inlined), so a run keyed on *known*
entries starts at the first slot that happens to resolve — generally in the **middle** of the
vtable. Dumping the bytes around a candidate showed it plainly: `0x803ab7a4` was reported as a
vtable, but `0x803ab788..0x803ab7a0` above it hold `0x803370e0…` — real code pointers merely missing
from the symbol list.

So every emitted address was an interior offset, **not the vtable base**. A guest vptr points at the
base, so the allowlist would have matched *nothing* — 100% false-negative, while printing 379
addresses and a plausible coverage percentage. Nothing downstream would have complained; actors
simply would not have interpolated.

### The fix, which also fixed the coverage

Key on "is this a valid `.text` pointer" instead of "is this a known function". Unknown slots no
longer break a run, so the run start IS the base, and known-function names are used only to
CLASSIFY. One change, both problems:

| | keyed on known funcs | keyed on .text pointers |
|---|---|---|
| candidates | 2,007 | 1,327 |
| TActor-tagged | 379 | **453** |
| decomp classes covered | 161/287 (56.1%) | **276/287 (96.2%)** |
| addresses usable as vptr | **no** (interior) | yes (base) |

Verified: **453 of 453** recovered bases have a non-`.text` word immediately before them, which is
what a real vtable start looks like (MWCC's zero/RTTI separator). 11 classes remain unmatched and
are listed by name — `TAnimalBase`, `TAirportPool`, `TCasinoRoulette`, `TMapObjGrassGroup` and 7
more — rather than absorbed into a percentage.

The tool ships `--selftest` (a case that MUST produce a positive), refuses on a missing DOL or
symbol file rather than returning an empty result, and computes its coverage denominator from the
decomp class hierarchy so "found 453" can never be mistaken for "found all".

### Open, and it must be settled before the allowlist is used

`TActor` is multiply derived (`TPlacement`, `JStage::TActor`), so a class has **more than one**
vtable. The samples show both shapes: a 7-slot table starting `load__TActor` / `perform__THitActor`,
and a 30-slot one starting with the six `JSG*` methods. Only the **primary** (the one a `+0x00`
vptr points at) is a valid membership key; seeding the allowlist with secondary tables reproduces
the same silent all-false-negative failure the base-address bug would have.

Distinguishing them is the next step: the primary is the table whose slot order matches the
`TNameRef -> TViewObj -> TPlacement -> TActor` virtual sequence the decomp headers declare, and the
secondary is the `JStage::TActor` interface reached through the `@32@` thunks that are already
visible in the US symbol list. **Failure-mode asymmetry makes this safe to iterate on**: a false
negative means an actor does not interpolate, whereas a false positive would write a transform into
a non-actor's memory — so the allowlist must be grown only as membership is proven, never guessed.

## VALIDATED: the TActor vtable allowlist, scored against ground truth — precision 47/47, recall 47/49

The static scan was wrong twice more, and both were caught by **running the discriminator against
real objects** instead of reasoning about multiple-inheritance layout. `diag_vptr.cpp`
(`SBR_VPTR_DUMP=1`) dumps the vtable pointer each live `TViewObj` actually carries, with its NameRef
name — 133 distinct vptrs over 200,001 dispatches, zero names unreadable.

### Error 1: off by 8 — every recovered address missed

First cross-check: **0 of 133** live vptrs matched *any* recovered candidate. Dumping raw words at a
real live vptr showed why:

    0x803c0620: 0x00000000     <- vptr points HERE
    0x803c0624: 0x00000000
    0x803c0628: 0x801576fc  __dt__10THelpActorFv        <- function pointers start at vptr+8

MWCC's vtable carries two zero words (offset-to-top, typeinfo — both zero with RTTI off) and the
object's vptr points **at them**. The scan reported the first *function* word, so every address was
+8 off. Correcting it — and requiring the two zero words, which also rejects ordinary
function-pointer tables — took the match to **133 of 133 (100%)**.

### Error 2: the TActor tag was tagging SECONDARY vtables

With addresses correct, only **4** of 133 were TActor-tagged, while `滝つぼ`, `バルーンヘルプ` and
`マップ` — objects the decomp's own snapshot roster had already **proven** are TActors — went
untagged. The tag keyed on a `Q26JDrama6TActor`-owned slot, and those are the `JSG*` methods, which
live in the **secondary** vtable (the `JStage::TActor` base). It was tagging secondary tables, never
the primary one a vptr points at.

The primary vtable instead carries the `TNameRef -> TViewObj -> TPlacement -> TActor` chain, whose
slots belong to the class itself or an ancestor. So the sound test is: **does any resolved slot
belong to a class the decomp says is TActor or derives from it?**

### Scoring, against both classes

| | count |
|---|---|
| live distinct vptrs | 133 |
| tagged TActor | 47 |
| of those, confirmed by the decomp's proven-TActor roster | 43 |
| remaining 4, confirmed by slot ownership (`THitActor`, `TSpineEnemy`, `TFruitsBoat`, `TBaseNPC`) | 4 |
| **false positives** | **0** |
| false negatives (`カモメ`, `水中カメラインダイレクト`) | 2 |

The four not in the decomp roster — 落書きグループ, フルーツ運搬船, ゲートキーパー（ビアンコ）,
モンテＮ — are genuine actors absent from the *decomp* scene because those plaza NPCs are known
unported gaps. Their presence in the recomp is the two-runtimes argument in miniature.

The negative side was checked too, which is the half that is usually skipped: `<FrmGXSet>`,
`<TOrthoProj>`, `graffito`, `落書き管理`, `SMS Draw Init` are all correctly **not** tagged.

### Usable now

`tools/re/us_vtables.py --emit-header` writes the allowlist. Precision is what safety depends on —
a false positive writes a transform into a non-actor's memory — and it is 47/47 on everything that
appears in a real Delfino frame. The two false negatives cost only that those objects do not
interpolate.

The type-identity blocker is cleared; the snapshot override can be written against this list.

## LANDED (recomp): the game-native (prev) snapshot runs, and it captures MARIO

`sms-recomp/overrides/interp60_snapshot.cpp` (`SBR_INTERP60=1`) owns the `testPerform` override and
snapshots each actor's transform before its movement. On Delfino with the stick driving Mario:

    SNAPSHOT: dispatches=3560510 (tactor=2944404 non-actor=616106) | compared=735690
              moved=12341 (1.7%) | maxStep all-time=2200.00 "陽炎"
              this-window=48.18 "マリオ"  evictions=0

* **`マリオ` is the largest mover in its window at 48.18 units/tick**, with `鳥 4/6/11` (seagulls)
  at 41-132. Real actors, plausible per-tick steps, so `(prev, cur)` is live and non-trivial.
* **616,106 non-actors correctly rejected** by the vtable allowlist — the type test is doing work,
  not waving everything through.
* **0 evictions**: the 8192-slot side table never overflowed, so no actor was silently dropped.
* Guest memory is **read only**; nothing is written yet.

### The per-window maximum was not a nicety

The all-time maximum sits at 2200 units on `陽炎` — the heat-haze prop's one init teleport, the same
artefact seen on the decomp side. It pins the all-time figure forever and would have hidden every
real mover behind it. Reporting a **per-window** maximum, reset each report, is what surfaced
`マリオ`. A single global max is a lossy summary of exactly the wrong statistic.

### Why this order

The snapshot is landed and proven before any interpolating write, because an inert `(prev, cur)`
pair renders identically to a working one: interpolation built on a dead snapshot would look correct
while doing nothing, and there is no later test that separates those two. `moved=1.7%` (~9 movers a
tick in a largely static plaza) with the player named among them is the evidence that the write has
something to interpolate.

### Next

Write `lerp(prev, cur, alpha)` into the guest transform for a sub-frame, re-run `PreEntry` + draw,
and present twice. The pieces are in place: the boundary is measured (pixel-identical re-run), the
present-twice seam already exists in `native_frame.cpp`, and the type test is validated. The first
check on the write side is the same shape as this one — with alpha pinned to 1.0 the frame must be
**pixel-identical** to no-interpolation, or the write path is wrong before interpolation is even
attempted.

## The interpolating WRITE lands — but the recomp frame is NOT DETERMINISTIC, so it cannot be verified yet

`SBR_INTERP60_ALPHA=<a>` now writes `lerp(prev, cur, alpha)` into the guest transform for the draw
phase and restores the game's own values before the next tick's movement, so physics never runs on
an interpolated pose. It fires: **1,969,101 applies** per run.

### Getting it to fire at all exposed one real defect

The first version gated `apply_all` on `e.tick == VIGetRetraceCount()` and reported
**`applied=0 restored=0`** — the write silently never happened. The draw-phase dispatch does not
reliably share the present counter's value with the movement dispatch (`direct()` has an
entry-vs-render alternation). Keying on the snapshot's OWN tick fixed it. Worth noting that the
counters are what surfaced this: without them the run would simply have produced a frame, and a
frame that looks right is indistinguishable from a write that never ran.

### The verification is INVALID, and the control is what says so

The gate was: `alpha=1.0` must be **pixel-identical** to no-write (identity), and `alpha=0.0` must
differ (the write reaches pixels). Results:

    alpha=1.0 vs base : 7612 of 1228800 (0.6195%)
    alpha=0.0 vs base : 6975 of 1228800 (0.5676%)

That reads as "identity broken". It is not — or at least, nothing here shows it. The control:

    BASELINE vs BASELINE, identical config, two runs: 6757 of 1228800 (0.5499%)

**The recomp frame at a fixed present count is not reproducible run to run**, and both alpha numbers
sit inside that noise. Neither the identity check nor its control can resolve anything.

Two things this invalidates, one of them mine:

* the verdict "identity STILL broken" — unsupported;
* a fix I had already applied and justified. `p + (c-p)*a` genuinely is not endpoint-exact in
  floating point, and I changed it to write the endpoints directly and use `(1-a)*p + a*c` between.
  That reasoning is sound and the code is better for it, **but the 7,273-pixel difference I cited as
  its evidence was noise.** The change is kept on its merits; the evidence claimed for it is
  withdrawn.

### Why the decomp could do this and the recomp cannot

The decomp side verified a re-run draw pass as **0 of 1,228,800 pixels** — bit-exact, repeatedly. So
determinism is not inherent to the game; it is something the recomp is losing. Prime suspect is a
time-seeded RNG (the plaza's birds, heat-haze and water are all random-driven, and this project has
already found `rand()` here resolving to libc rather than the GC LCG), but that is a hypothesis and
naming it is not measuring it.

### Next, and it is a TOOLING task before it is an interpolation task

Pixel-exact A/B is the only instrument that can validate the write path, and it does not currently
work in the recomp. So: find and pin the non-determinism (a fixed RNG seed, or whatever the source
turns out to be), prove it with baseline-vs-baseline reaching **0 differing pixels**, and only then
re-run the identity/control pair. Building interpolation further on an unverifiable write would be
exactly the "looks correct while doing nothing" failure this whole arc has been guarding against.

The write path is **UNVERIFIED** — not correct, not broken. It stays gated off by default.

## SOLVED: the recomp is deterministic — the host clock was reaching the guest

Pixel-exact A/B now works: **0 of 1,228,800 differing pixels** between two runs, with the scene
LIVE (`moved=1.9%`, birds stepping 123 units/tick).

### Root cause

`tb_get()` (`sms-recomp/runtime/rt_core.cpp`) derived the Gekko time base from
`clock_gettime(CLOCK_MONOTONIC)`. Every `OSGetTime` the game made therefore differed run to run, and
everything seeded from or timed against it diverged. `SBR_DETERMINISTIC=1` substitutes a virtual
clock advancing one nominal NTSC field per present.

### Three wrong turns, each caught by a check rather than by reasoning

1. **The spatial map pointed at THP first.** The differing pixels were not scattered — they formed a
   compact upper-left blob. `SBR_THP=none` cut them 6,757 -> 2,165, which looked like a hit.
2. **But `THP=none` FREEZES THE PLAZA.** The interpolation probe reported `moved=0.0%` under it,
   against 1.6% normally, and the object mix changed fivefold (non-actors 616k -> 3.09M). So
   "`THP=none` + virtual clock gives 0 differing pixels" was largely **a static scene**, not a
   deterministic one — a false success that a pixel comparison alone would have certified. Only
   pairing determinism with a LIVENESS number caught it. The virtual clock alone, with THP left on,
   is what actually works; disabling THP was never needed.
3. **The first virtual clock hung the game.** It returned `frame*step + (++calls % step)`; the
   modulo wraps, so the clock ran BACKWARD mid-frame, and guest code spinning until the time base
   reaches a deadline never finished — the run stalled at THP open and never reached gameplay. A
   time base here must be monotonic AND able to advance without a present, since a spin-wait issues
   none. It is now a frame+call candidate floored to strictly exceed the previous value.

### And the interpolation write is still not verified — but now for a KNOWN reason

With the harness fixed, the identity/control pair reads:

* `alpha=1.0` vs baseline: **0 differing pixels** — identity exact, the write is a true no-op.
* `alpha=0.0` vs baseline: **0 differing pixels** — the control STILL FAILS.

The first apply point was wrong and the control is what said so: `calcRootMatrix()` turns
mPosition/mRotation into the model's base TRMtx during **CUE_CALC_ANIM (0x2)**, which precedes entry
and draw. Applying at the first draw-side dispatch writes into memory nothing reads again that
frame. Moving the apply ahead of the calc phase was correct and necessary — but the control still
does not fire, so something further along still consumes a pose the substitution does not reach.

Note what identity alone would have claimed here: `alpha=1.0 -> 0 pixels` passed at every stage,
including while the write was landing in dead memory. **A no-op check cannot distinguish a correct
write from an ineffective one**, which is precisely why the alpha=0.0 control exists.

Next: find what the substitution has to precede. Candidates, in order — the pose may be consumed
before CUE_CALC_ANIM by an earlier phase, the actors that draw may not be the ones being written
(the allowlist covers 47/47 of a live frame, but only ~1.9% move), or `TMario` may compute its
matrix outside the phase entirely. Each is checkable; none should be assumed.

## The write DOES reach the render — the RESTORE POINT is wrong

Three probes, each narrowing the previous one's ambiguity, and none of it reachable by reasoning.

**1. `alpha=0.0` changed 0 pixels.** Ambiguous: either the write reaches nothing, or `(prev, cur)`
are equal for everything on screen.

**2. `SBR_INTERP60_KICK=3000` — a 3,000-unit displacement of every allowlisted actor — also changed
0 pixels.** That removes the second reading: a displacement that large cannot be invisible if it is
reaching rendered state.

**3. Read-back: `stuck=1968690 lost=0`.** Every store lands in the memory the guest reads. So the
write is not being dropped — it is being *ignored*, which is a different defect and would have been
mis-diagnosed as a memory-mapping problem without this check.

**4. `SBR_INTERP60_NORESTORE=1` — leave the substitution permanently in place: 1,201,698 of
1,228,800 pixels change (97.8%).**

So the write reaches rendered state completely. **`restore_all()` simply runs before the render
consumes the pose.** It is placed at the first `CUE_MOVE` dispatch of the next tick, and the draw
for tick N evidently happens *after* that point — consistent with the entry-vs-render alternation
`MarDirectorDirect.cpp` documents, where a direct() call renders what a previous call entered.

### What this retracts

The previous entry concluded the apply point was wrong because `calcRootMatrix` runs on
`CUE_CALC_ANIM` before draw. Moving the apply ahead of the calc phase is still correct on its own
terms, **but it was not the reason the control failed** — the control kept failing after that change
and the real cause is the restore, at the other end. A fix that does not move the symptom is not the
fix, and that should have been the signal to stop and re-measure rather than reason on.

### Next

The restore has to happen after the frame's GX stream is complete and before the next tick's
movement. The present boundary (`native_frame.cpp`'s `present_and_reopen`) is the unambiguous point
— but note that the naive expectation "the draw precedes the present" is exactly what the evidence
just contradicted, so the placement must be **measured, not assumed**: `NORESTORE` at 97.8% and a
correct placement at 0% for `alpha=1.0` with a firing `alpha=0.0` control is the gate.

Worth recording what the single-present experiment actually is: a degenerate stand-in for the real
design, which never restores mid-frame at all — it presents TWICE, the extra present carrying the
interpolated pose. The restore question may dissolve once the second present is wired, and that is
the more likely shape of the fix than moving the restore around.

## MEASURED: the phase order inside a direct() call — DRAW COMES FIRST

Two apply/restore placements were derived by reasoning about the tick's phase order and both were
wrong. `SBR_INTERP60_TRACE=1` prints the dispatch masks in order with the present boundary marked,
and the order is:

    draw (0x8) ... 0x80, 0x10, 0x1000000
    movement (0x1) ... 0x2001, 0x1001, 0x3001, 0x40000001
    calc (0x2) ... 0x3, 0x6, 0x40000002
    view (0x4) ... 0x10, 0x4
    entry (0x200 / 0x400) ... 0x480, 0x204, 0x4000200, 0x2000200
    PRESENT

**A direct() call DRAWS FIRST**, rendering what the *previous* call entered, and only then runs
movement, calc, view and entry for the new state. The present follows all of it. That is the
entry-vs-render alternation `MarDirectorDirect.cpp` documents, now in concrete dispatch order rather
than prose.

### Both failed placements explained

* **Restore at the next tick's first movement dispatch** (original): produced 0 pixel change with a
  substitution active, while disabling restore entirely produced 97.8%. Still not fully explained by
  the order above — movement follows draw within a call, so the restore *should* land after the draw
  that matters. Something between calc and the consuming draw is still not understood, and saying so
  is more useful than another guess.
* **Restore at the present boundary**: catastrophic and cleanly diagnosed. The present is AFTER that
  call's movement, so restoring there writes stale values over movement the game has already done.
  `moved` collapsed from **1.9% to 0.0%** — every actor frozen — and both alphas then produced an
  identical frame differing from baseline by 98.6%, which is a broken game, not interpolation. The
  liveness counter is what caught it; a pixel diff alone would have shown a large confident-looking
  difference.

Reverted to the movement-dispatch restore, which keeps the game correct and moving (`moved=1.8%`).

### What this leaves

The write path is proven to reach the render (97.8% with restore off). The phase order is now
measured rather than assumed. What remains unknown is precisely which draw consumes the pose the
calc phase bakes into the model matrix, and therefore where a restore can sit without either being
too early (invisible) or too late (clobbering movement).

The next step is bisection over the measured order, not another derivation: move the restore to each
candidate boundary in turn and read the identity/control pair, which is now a trustworthy instrument
(deterministic frames, 0-pixel baselines). Three reasoned placements have failed; the order is
cheap to observe and expensive to guess, and that lesson is the finding here.

## VERIFIED: the interpolation write path works — bracket the DRAW BLOCK, do not mutate-and-undo

    alpha=1.0 vs baseline :   0 of 1,228,800  (identity exact)
    alpha=0.0 vs baseline : 162 of 1,228,800  (control FIRES)
    liveness              : moved=1.8%        (movement uncorrupted)

Both halves of the gate pass at once for the first time.

### What was wrong, and it was a SHAPE problem not a placement problem

Three attempts substituted the pose at one phase and undid it at another — mutate-and-undo spanning
phase boundaries. Each needed exact knowledge of every consumer of the pose in between, and each was
wrong in a different way: invisible (restore before the consuming draw), or catastrophic (restore at
the present, overwriting movement the game had already done and freezing every actor).

The user's reaction to the design — *"idk what restore is but it sounds like the wrong approach"* —
was correct, and identified the real defect faster than three more measurements would have. A
separate undo step is not part of the design; it was an artefact of testing with a SINGLE present.

### The fix

The measured dispatch order shows the draw dispatches form a **contiguous block at the start of a
direct() call**, before movement:

    draw (0x8) ... -> movement (0x1) -> calc (0x2) -> view (0x4) -> entry (0x200) -> PRESENT

So the substitution opens at the first draw dispatch and closes at the first non-draw dispatch after
it. It never outlives the block that consumes it, and no phase that reads the pose for anything
other than drawing ever sees a substituted value. Nothing has to be known about consumers outside
the block, because the substitution does not reach them.

### And the undo disappears entirely in the real design

This is still a stand-in. The actual design renders the tick TWICE — sub-frame A at `alpha=0.5`,
sub-frame B at `alpha=1.0` — and needs no undo at all, because the last pass writes the true state
by construction. The bracket above is the single-present degenerate case of that, and it is what
made the write path verifiable before the second present exists.

### Note on the control's magnitude

162 pixels is a small but unambiguous signal: at `alpha=0.0` only the ~20 actors that move in a tick
shift, and at this camera they are small on screen. It is the SIGN that matters here (0 vs non-zero
against a 0-pixel-noise baseline), and the earlier `SBR_INTERP60_KICK=3000` probe already
established the path can move 97.8% of the frame when the displacement is large.

**Next: the second present.** The pieces are all verified — snapshot (coverage-proven), write path
(identity + control), deterministic frames, and a measured phase order to insert into.

## RETRACTED: the draw-block bracket reaches NOTHING — reach=0 under a 3000-unit kick

The previous entry declared the write path VERIFIED on an identity/control pair:

    alpha=1.0 vs baseline :   0 of 1,228,800   identity exact
    alpha=0.0 vs baseline : 162 of 1,228,800   control fires

Adding a **reach** probe to the same gate (`tools/interp/interp60_gate.sh --kick 3000`, which
displaces every snapshotted actor 3000 units in Y inside the same bracket) gives:

    identity  alpha=1.0            :   0 of 1,228,800
    control   alpha=0.0            : 273 of 1,228,800
    reach     alpha=1.0 kick=3000  :   0 of 1,228,800     <-- ZERO
    write     read-back            : stuck=46,271,654 lost=0

Every one of 46 million writes lands in memory the guest reads back, and moving every actor three
thousand units changes **not one pixel**. The bracket does not enclose anything that renders from
`mPosition`. The 273-pixel control was a sliver — and since the kick writes only `pos.y` while the
`alpha=0.0` path writes position *and* rotation, those 273 pixels are most likely rotation-driven,
i.e. not even evidence for the position path.

### Why the pair passed anyway, and what that says about the instrument

Identity and control are a *correctness* pair, not a *coverage* pair. Together they prove the write
is exact where it lands and lands somewhere rendered. Neither can say the bracket covers the scene,
and this project's own rule says an instrument must declare what it does not cover. It did not, so
"control fires" was read as "the mechanism works". The gate now runs the reach probe alongside them
and prints the coverage caveat under the verdict.

### The root cause: the bracket was built on a truncated trace

`SBR_INTERP60_TRACE` started at boot (loading screen) and capped at 90 transitions, so it truncated
inside the first tick. The sequence it printed was read as the tick's phase order and put the DRAW
lists at the *start* of a `direct()` call — the opposite of what `MarDirectorDirect.cpp` actually
does (movement -> calcAnim -> **PreEntry** -> `mPerformListDrawBufGroup` / `Graffito` / `Pollution` /
`GX` / `Silhouette` / `GXPost`). Compounding it, those draw lists dispatch with mask `0xffffffff`,
so a `mask & 0x8` test matches them *and* matches almost everything else: a test that cannot
separate the phases cannot place a bracket. The bracket opened on an early `0x8` group and closed on
the next `0x80`, covering none of the draw lists.

The trace now starts after the scene is live (`SBR_INTERP60_TRACE_AT`, default present 800), counts
how many dispatches each collapsed run represents, and says so explicitly when it truncates.

### This was already answered, higher up in this same file

The `SB_DOUBLE_DRAW` probe measured it on the decomp side and the entry is above: **the sub-frame
boundary is `PreEntry` + draw, not draw alone** — a re-run of `PreEntry` + the draw lists reproduces
the frame pixel-identically, and dropping `PreEntry` moves 1,956 pixels. Re-deriving a worse answer
than one already recorded in the same document is a workflow defect, not just a wrong measurement.

### And the mechanism already exists in git — resurrect, do not rebuild

`git show 9283f44^:runtime/overrides/interp_redraw.cpp` (754 lines, recomp era) is a working second
present built exactly this way. It re-issues the draw lists off `gpMarDirector`:

    kDrawLists = { 0x40 DrawBufGroup, 0x38 Graffito, 0x3C Pollution,
                   0x1C GX,           0x20 Silhouette, 0x24 GXPost }

reconstructing a `TGraphics` on the stack, calling `TPerformList::perform` per list with a mask that
**drops `0x1` movement and `0x2` calc-anim** (re-running those double-steps water scroll and other
per-frame animation — RE'd, with the flicker it caused), then `GXInvalidateTexAll`, then steering the
display's XFB pointer so the in-between field copies to its own buffer. It also blends and restores
the j3dSys view matrix (`0x804045DC`) and `gpMarioPos`, because the drop shadow projects from the
live global rather than from a draw matrix.

Two things it did NOT do, both of which the current design should: re-run `PreEntry` (measured above
as worth 1,956 pixels), and interpolate in the game's own actor fields rather than by blending
`mDrawMtxBuf` — the matrix-blend approach is precisely the one the standing directive rejects,
because effects with no model matrix step at 30 Hz under it.

### Next

Resurrect the list re-issue against the current runtime, with `PreEntry` in the re-issue set and the
snapshot substitution wrapping it. The bracket then becomes the re-issue itself: substitute, re-issue
`PreEntry` + draw lists, present, restore — and in the two-present shape the final pass writes the
true state, so there is nothing to undo.

## THE SECOND PRESENT EXISTS AND RUNS — and the one thing blocking it is named

`sbr_interp60_subframe` (overrides/interp60_snapshot.cpp) is called from the frame seam right after
the tick's own present and re-issues the tick's draw lists at an interpolated pose. It renders:
**3,296 sub-frames** over a run, with the game still live.

### The tick, measured rather than assumed

`SBR_INTERP60_LISTS=1` prints the ordered outermost `TPerformList::perform` calls of a tick. It is
the same 15 calls every tick:

    #0 DrawBufGroup(1)  #1 Graffito(7)  #2 Pollution(14)  #3 GX(108)  #4 GXPost(534)
    #5..#12 Movement / unk30, four cue-masked pairs (~477 / 16 dispatches each)
    #13 CalcAnim(470)   #14 PreEntry(487)   PRESENT

So the draw lists really do run FIRST and PreEntry LAST — the entry-vs-render alternation, now in
call-by-call form. The frame a seam presents was drawn from the pose entered one tick earlier, which
is what makes the seam the temporally correct place for the in-between: at tick N's seam the
presented image is N-1, the sub-frame is lerp(N-1, N, alpha), and the display sequence comes out
N-1, mid, N, mid, N+1 — monotonic, not a sawtooth.

### gpMarDirector found by scan, not by symbol

The re-issue set has to be named by FIELD (PreEntry @ +0x34, DrawBufGroup @ +0x40, Graffito @ +0x38,
Pollution @ +0x3C, GX @ +0x1C, Silhouette @ +0x20, GXPost @ +0x24), and gpMarDirector's US address is
not in `reference/sms_gmse01_funcs.txt` (the JP symbol 0x8040A2A8 does not carry over). Rather than
trust a constant, the director is found as the unique object holding the observed list pointers at
those offsets:

    TMarDirector scan: 6,291,438 words examined, 1 candidate matched all four anchor slots
    gpMarDirector = 0x808f2a40

with every field then dumped and cross-checked against what was performed. One candidate out of six
million words is a stronger identification than a symbol constant, and it re-derives itself on any
build.

The re-issue set is RECORDED from the tick rather than hardcoded: the run of outermost list performs
before the director's own Movement list. Silhouette is only performed when its gate is open, so a
fixed set would either draw what the tick did not or miss what it did.

### The blocker: re-running PreEntry into full draw buffers never returns

With PreEntry in the re-issue the sub-frame wedges inside GXPost. It is not a deadlock and not a
stall — the stream instrument shows it emitting and advancing (GX list 1,185 KB, then GXPost past
1,664 KB and climbing, grinding in `J3DShape::draw`), and the fifo-stall probe (`SBR_FIFO_STALL`)
never fires, so the parser is making progress the whole time.

`SBR_INTERP60_NOENTRY=1` — re-issue the draw lists WITHOUT the extra PreEntry — is the discriminator,
and it is decisive: **3,296 sub-frames rendered, no hang.**

The cause follows from the measured order. PreEntry is the LAST list of a tick, so by the time the
seam runs, the draw buffers are already full of tick N's entries. Draw buffers are populated by
entry and *consumed* as they are drawn (measured earlier in this file via `SB_DOUBLE_DRAW`); they
are not rebuilt. A second PreEntry therefore appends a second set into buffers that were never
emptied, and the draw that follows walks a structure that no longer terminates in the time a frame
has.

### What this needs next, precisely

A draw-buffer RESET between entries. The sequence becomes:

    reset buffers -> PreEntry(mid pose) -> draw lists -> present
    reset buffers -> PreEntry(true pose)              [leaves tick N entered for the next tick]

That is an RE task with a clear target: how the game itself empties the draw buffers each frame
(`J3DDrawBuffer` frame-init, and whichever object in `mPerformListDrawBufGroup` — one dispatch, #0 —
drives it). It is NOT a case for a workaround: without the reset the sub-frame draws the entries
tick N already had, which is the same picture presented twice.

`SBR_INTERP60_NOENTRY=1` runs today and is honest about what it is — a sub-frame that re-draws
tick N's entries at an interpolated pose that most geometry does not read. It is a harness for the
re-issue plumbing, not 60fps.

## RE ANSWER: the scene RESETS AND RE-ENTERS ITSELF on the draw cue — no PreEntry re-run wanted

The blocker named in the previous entry ("a draw-buffer reset is needed before re-entry") had a
better answer than the one it proposed: **the reset is already inside the draw cue.**

`JDrama::TSmJ3DScn::perform` (decomp `src/JSystem/JDrama/JDRSmJ3DScn.cpp:46`):

```c
if (param_1 & 8) {
    if (mLightMap) mLightMap->perform(0x20, param_2);
    MTXCopy(param_2->mViewMtx, j3dSys.getViewMtx());
    j3dSys.drawInit();
    for (int i = 0; i < mDrawBufferCount; ++i) mDrawBuffers[i]->frameInit();   // RESET
    j3dSys.setDrawBuffer(mDrawBuffers[0], 0);
    j3dSys.setDrawBuffer(mDrawBuffers[1], 1);
    TViewObjPtrListT::perform(param_1 | 0x204, param_2);                       // ENTER
    j3dSys.setUnk4C(3); mDrawBuffers[0]->draw();
    j3dSys.setUnk4C(4); mDrawBuffers[1]->draw();                               // DRAW
}
```

Reset, enter and draw are ONE unit under cue `0x8`, and the entry cue it forwards (`0x204`) is
generated from within. `J3DDrawBuffer::frameInit` is just `mBuffer[i] = nullptr` for `i < mSize`
plus `mCallBackPacket = nullptr` (J3DDrawBuffer.cpp:90). `TDrawBufObj::perform` splits the same
three across cue bits for the non-scene buffers — `0x80` frameInit, `0x400` setDrawBuffer, `0x8`
draw — which is what the `0x80` in the draw block's dispatch trace always was.

So re-issuing the draw lists re-enters every scene object **from whatever pose is live at that
moment**, which is precisely the interpolated one this file just wrote. And it is self-healing: the
next tick's render branch runs the same reset-enter-draw from the restored pose, so no undo exists
anywhere in the design.

### Measured, with the probe that caught the last mistake

    reach, sub-frame re-issue WITHOUT PreEntry, kick=3000 : 1,193,906 of 1,228,800  (97.16%)
    reach, the earlier draw-phase bracket, kick=3000      :         0 of 1,228,800  ( 0.00%)

97% of the frame moves. The substitution reaches the scene.

### Why the extra PreEntry hung, precisely

PreEntry is the LAST list of a tick and its entries are consumed by the NEXT call's render branch —
the entry-vs-render alternation `MarDirectorDirect.cpp` documents in its own comments (ENTRY-side
branch ~line 162, RENDER-side/else ~line 275, "the first loop iteration of every direct() call takes
the else-branch (catch-up render)"). The seam therefore finds the draw buffers FULL of tick N's
entries, not empty. A second PreEntry appended a second set into them, and the draw that followed
did not finish in any time a frame has.

That also confirms the ordering claim the sub-frame's placement rests on, now from decomp source
rather than from a trace: `direct()` renders what a previous call entered, `TDisplay::endRendering`
(and with it our seam) runs after the whole of `direct()`, so the frame presented at tick N's seam
is the pose entered at N-1 and the sub-frame built there falls between them.

`SBR_INTERP60_NOENTRY` is gone; not re-running PreEntry is now the behaviour. `SBR_INTERP60_PREENTRY=1`
restores the old path for A/B and is expected to wedge — a fault this specific is worth keeping
reproducible.

## IDENTITY, MEASURED INSIDE ONE RUN: the sub-frame reproduces the frame PIXEL-FOR-PIXEL

    series of 4 consecutive presents, SBR_INTERP60_ALPHA=0.0
      present 0 -> 1 :  349,199 of 1,228,800 (28.42%)
      present 1 -> 2 :        0 of 1,228,800 ( 0.00%)   <- sub-frame reproduces its main neighbour
      present 2 -> 3 :  350,689 of 1,228,800 (28.54%)

The zero is the identity: a sub-frame at `alpha=0.0` re-enters the pose the main present already
drew, and the re-issue reproduces that frame **bit for bit**. This is the recomp's restatement of
the decomp's `SB_DOUBLE_DRAW=3` result, and it says the re-issue path — reset, re-enter, re-draw,
present — is faithful, not merely plausible. The ~28% either side is one tick of real motion between
successive main frames, which is also the scale a midpoint has to land inside.

### The instrument had to be rebuilt first, and the reason is the same one as last time

`SB_DUMP_FRAME_AFTER` counts PRESENTS. The sub-frame adds a present per tick, so the same index
reaches a DIFFERENT game moment once interpolation is on — two configurations dumped "at frame 1500"
are not the same moment, and a diff between them is dominated by that drift. Scoring `alpha=1.0`
against a no-sub-frame baseline that way would have produced a large confident number meaning
nothing, which is exactly the shape of the mistake this file has already recorded twice.

So the comparison moved INSIDE one run: `SB_DUMP_FRAME_EVERY=1` with a new
`SB_DUMP_FRAME_COUNT=N` (aurora) captures N consecutive presents, which alternate main/sub, and
`tools/interp/subframe_gate.py` reports every adjacent pair plus the alternation pattern rather than
picking a pairing. Nothing can drift, because there is only one run.

The gate's two halves are now derived from the measured phase order rather than assumed:

    alpha = 0.0  ->  sub-frame must REPRODUCE its main neighbour   (identity)  CONFIRMED, 0 px
    alpha = 1.0  ->  sub-frame must DIFFER, by one tick of motion  (control)

## THE CONTROL DOES NOT FIRE — and the RE says why: THE CAMERA IS NOT A TActor

`alpha=1.0` produced byte-identical output to `alpha=0.0`:

    alpha=0.0 : 349,199 | 0 | 350,689
    alpha=1.0 : 349,199 | 0 | 350,689     <- identical, to the pixel

Alpha has no effect on the image, while a 3000-unit kick moves 97% of it. Writes reach the render;
the interpolation endpoints do not change it. That contradiction has a structural explanation.

`JDrama::TCamera : public TPlacement, public JStage::TCamera` (JDRCamera.hpp:9) — **not TActor.**
`kTActorVtables` is generated from the TActor hierarchy, so cameras were never in the allowlist,
never snapshotted, and never substituted. Meanwhile `TLookAtCamera::perform` rebuilds the view from
the camera's own fields on cue `0x14`:

    if (!(param_1 & 0x14)) return;
    C_MTXPerspective(param_2->mProjMtx.mMtx, mFovy, mAspect, mNear, mFar);
    C_MTXLookAt(param_2->mViewMtx, &mPosition, &mUp, &mTarget);

and `TSmJ3DScn::perform` then copies that view into j3dSys. The sub-frame's re-issue cue keeps
`0x4`/`0x10`, so the camera DOES recompute — from an un-substituted pose. Between two ticks of a
walking player at this camera, ~28% of pixels change and that motion is overwhelmingly the CAMERA;
the actors that move are 2.5% of the roster and small on screen. So substituting actor transforms
alone is very nearly invisible, exactly as measured.

This is a coverage gap in the snapshot, not a fault in the sub-frame. The sub-frame reproduces the
frame pixel-for-pixel (previous entry) and the write path reaches 97% of it under a kick; what is
missing is that the thing which actually moves the picture was not in the set being interpolated.

### Next

Snapshot the camera in its own terms — `mPosition` (+0x10 via TPlacement), plus `mTarget` and `mUp`
which `C_MTXLookAt` reads — and substitute them with the same prev/cur machinery. `us_vtables.py`
already recovers vtables by hierarchy ownership; it needs a second root (`JDrama::TCamera`) and a
second emitted list. Nothing about the sub-frame changes: the camera's `perform` is already
re-issued with a view-bearing cue, so an interpolated camera pose flows straight into `mViewMtx`.

## Instrument note: two bugs the apply-time counter and the refusal counter caught immediately

* **The TGraphics snapshot was captured once, ever** (`if (!g_gfxValid)`), so every sub-frame after
  the first rendered through the first tick's camera — `TSmJ3DScn::perform` copies `mViewMtx` out of
  that struct. Now refreshed per tick.
* **Resetting that snapshot at the START of the seam broke the sub-frame entirely.** The draw block
  is recorded EARLY in a `direct()` call and the seam runs at the END of it, so clearing on entry
  discards the recording being used: `rendered=0 refused=1200`. The reset belongs after the
  sub-frame consumes it. The refusal counter turned what would have been a silent 30fps run into an
  immediate, unambiguous number — which is the whole reason it prints its denominator.

## RETRACTED: "reach 97.16%" was the SAME cadence artifact, and no pose test has moved a pixel yet

The reach measurement recorded above — "1,193,906 of 1,228,800 (97.16%) under a 3000-unit kick" —
compared a run WITH sub-frames against a run WITHOUT them, both dumped at present 1500. The
sub-frame adds a present per tick, so present 1500 is tick ~750 in one and tick ~1500 in the other.
**That number is two different game moments, not a reach.** It is the identical defect this file had
already identified one entry earlier, applied to a measurement taken before the fix and not re-taken
after. Recording it as evidence was wrong; it is withdrawn.

With the within-run gate — which cannot drift, because there is only one run — every pose-based
configuration produces the SAME THREE NUMBERS, to the pixel:

    alpha=0.0, cue ~0x3   : 349,199 | 0 | 350,689
    alpha=1.0, cue ~0x3   : 349,199 | 0 | 350,689
    alpha=0.5, cue ~0x3   : 349,199 | 0 | 350,689
    alpha=0.5, cue ~0x1   : 349,199 | 0 | 350,689     (0x2 restored, calcRootMatrix re-runs)

Four configurations, byte-identical output. The sub-frame is always equal to its main neighbour and
the frames do not depend on the interpolation at all — including with cue `0x2` present, so the
`calcRootMatrix` explanation, while correct about what `0x2` does, did not change the outcome.

### What that leaves, and the control that separates it

Two hypotheses remain and the pixel data cannot tell them apart:

1. the substitution reaches nothing that draws, or
2. the sub-frame's GX stream never reaches the present — the main frame is simply presented twice.

`SBR_INTERP60_DROPLAST=1` omits a whole draw list from the re-issue. It involves no poses at all: if
the sub-frame is rendered from its own stream, a missing list MUST change the image. If the pair is
still 0 with a list missing, hypothesis 2 is the answer and every pose measurement taken so far was
measuring a duplicated frame.

Designing that control took one line and should have come before any pose test — "sub-frame equals
main frame" was ambiguous from the first measurement, and four runs were spent inside the ambiguity.

## THE PLUMBING CONTROL FIRES — and it demolishes the reading of every number before it

`SBR_INTERP60_DROPLAST=1` (omit one draw list from the re-issue, no poses involved):

    with the full re-issue    : 349,199 | 0 | 350,689
    with one draw list dropped:     193 | 0 |     168

Dropping a list from the SUB-frame collapsed the difference between MAIN frames from 28% to 0.016%.
A sub-frame cannot change how much the game moves between ticks, so **the 28% was never game
motion**. It was the sub-frame's re-issue of GXPost perturbing alternate presents.

So the reading attached to every previous measurement — "~28% either side is one tick of real
motion, which is the scale a midpoint has to land inside" — was wrong. The scene at this moment is
nearly static (~190 px between presents once the perturbation is removed), and the "identity 0 px"
reported as a success is equally consistent with a duplicated frame in a nearly-static scene. That
success claim is withdrawn along with the reach number.

### The control that was missing from the start

Nowhere in this arc did anything measure **how much the game changes per tick with interpolation
switched off**. That is the denominator for all of it: an interpolated midpoint can only be
demonstrated against a known inter-tick delta, and "sub-frame equals main frame" means nothing
without knowing whether consecutive ticks differ at all. Every gate here compared the sub-frame to
its neighbour and none established the neighbour's own motion.

Running it now (`SBR_INTERP60` unset, same pad script, same 4 consecutive presents — which are then
consecutive game ticks). Two outcomes, both informative:

* consecutive ticks differ substantially -> the scene does move, and a sub-frame equal to its
  neighbour means the substitution genuinely reaches nothing;
* consecutive ticks barely differ -> the test scene is static and NO pose measurement taken in this
  arc could have shown anything. The pad script would then be the defect: it drives a stick but was
  never confirmed to produce motion at the dump moment, and every "no effect" result so far is
  uninterpretable rather than negative.

This is the fourth time in this arc that a measurement was read as a result before its control
existed. The pattern is specific enough to name: the gate compares A against B and reports their
difference, but nothing establishes that B is what it is assumed to be.

## CORRECTION to the entry above: the plumbing WORKS, and the pairs were mislabelled

The baseline finally exists — `SBR_INTERP60` unset, so consecutive presents ARE consecutive game
ticks:

    tick -> tick : 1,064,307 (86.6%) | 1,068,851 (87.0%) | 1,075,670 (87.5%)

The game changes ~87% of the frame per tick. It is moving hard, and the previous entry's conclusion
("the scene is nearly static, the 28% was not motion") was wrong. It came from reading the
`subframe_gate` pairs as (main,main) when they are (main,sub),(sub,main),(main,sub).

Read correctly, the sub-frame run says:

    main_A -> sub_A : 349,199      the sub-frame DIFFERS from the main frame before it
    sub_A -> main_B :       0      and is IDENTICAL to the main frame after it
    main_B -> sub_B : 350,689

`sub_A == main_B` exactly. The sub-frame is **not** a duplicated image — it is a genuine render, and
`SBR_INTERP60_DROPLAST` proves the image comes from the sub-frame's own stream (removing one draw
list moves that 349,199 to 193). **The re-issue plumbing is correct.** The "identity 0 px" result
withdrawn in the previous entry is withdrawn for a different reason than stated there: the zero is
real, but it is against the FOLLOWING main frame, not the preceding one, so it says the sub-frame
renders pose N — which is `alpha=1.0` behaviour at every alpha.

### The actual defect is COVERAGE, and the apply-time counter already measured it

    AT APPLY TIME: 602,888 entries substituted, 13,597 had prev != cur (2.3%),
                   largest delta 885.66 "ゲーム看板8"

Across ~1,500 ticks that is roughly **nine objects per tick** whose position actually changed, and
the largest mover is a signboard. Substituting nine small props cannot visibly alter a frame whose
inter-tick delta is 87% — that 87% is the CAMERA and the player, and:

* the camera is excluded by class (`JDrama::TCamera : public TPlacement`, not `TActor`), as the
  earlier entry established; and
* whatever is driving the rest of the 87% is not in the substituted set either — nine movers out of
  ~400 snapshotted objects per tick is not a plausible account of a scene in motion.

So the sub-frame renders pose N because the substitution barely changes the live pose: the things it
covers are not the things that move. Nothing is wrong with the re-issue, the cue, the write path or
the present. The next work is coverage — the camera first, then confirming the player is actually in
the table and substituted (the roster contained マリオ, but the apply-time maximum is a signboard,
which is a discrepancy the roster/apply pair can be made to answer directly).

### The pattern, restated more precisely than "missing control"

Both of the last two wrong readings came from the same place: the gate prints a difference between
two frames and NAMES NEITHER. `subframe_gate.py` reports `present k -> k+1` without saying which of
those is a main frame and which is a sub-frame, so every interpretation was supplied by me rather
than by the instrument — and twice it was supplied wrongly. An instrument that reports a difference
between two things it cannot identify is not measuring what it appears to measure. The tool must
label main vs sub from the runtime, not leave it to be inferred from the pattern of zeros.

## THE INSTRUMENT NOW LABELS ITSELF — and the labels overturned the previous entry AGAIN

`aurora_set_dump_tag` (new, aurora) stamps a role onto each dump's filename; `native_frame` sets
`"main"` before the tick present and `"sub"` before the sub-frame present. The same series, now
self-describing:

    sub  -> main : 349,199 (28.4%)
    main -> sub  :       0 (0.0%)      <- the sub-frame equals the main frame BEFORE it
    sub  -> main : 350,689 (28.5%)

The zero is against the PRECEDING main frame. The previous entry concluded the opposite (`sub_A ==
main_B`, "renders pose N") from the same numbers, unlabelled. Two readings of one dataset, both
confident, both derived from a pattern of zeros rather than from any identification — which is
exactly why the labels now come from the runtime and why `subframe_gate.py` refuses to interpret an
unlabelled series.

### The stream handoff is CORRECT — measured, not assumed

    sub-frame present #1: g_out 1775 KB before build, 0 KB after; g_last now 1776 KB

Every sub-frame builds and sends **1.7 MB of its own commands**. `gxfifo_build()` returns early on
an empty `g_out` (which would leave `g_last` holding the main frame and silently re-send it) — that
is not happening. The sub-frame is a genuine re-render, not a duplicated present.

So the state of the mechanism is: **the re-issue reproduces the frame pixel-for-pixel from its own
1.7 MB stream.** That is the faithfulness property the design needs, and it is now supported by
stream evidence rather than by a bare pixel zero, which a duplicate would also have produced. What
it does NOT yet do is differ by alpha, because the substitution covers nothing that moves.

## THE PLAYER-COVERAGE PROBE READ THE WRONG ADDRESS — and said so itself

    PLAYER COVERAGE: gpMarioOriginal -> 0x8136384c "<unreadable>" vptr=0x00000000
                     is_tactor=NO in_snapshot_table=NO
      pos@+0x00 = (0.00, 300.00, 7400.00)   pos@+0x10 = (-0.00, 1.00, 1.00)

`vptr = 0` and an unreadable TNameRef name mean this is not an object. `+0x00` holds a plausible
world position and `+0x10` holds `(0, 1, 1)` — a SCALE. `JDrama::TPlacement` keeps `mPosition` at
`+0x10`, so whatever this is, it is not a TPlacement: **0x8040E10C points at a bare TVec3, not at
the TMario object.**

Therefore the line "the player is never snapshotted and never substituted" is **NOT a finding** —
it is the probe reading the wrong address, and the coverage question remains open. It was caught
only because the probe prints its raw evidence (vptr, name, both candidate position fields) beside
its verdict instead of the verdict alone. A probe that had printed just `is_tactor=NO` would have
produced a fourth false finding in this session, and it would have looked exactly like the real
thing.

The address needs re-deriving before the coverage question can be asked again: `0x8040E10C` is a
position pointer (consistent with the `gpMarioPos` RE in the memory notes), and what is needed is
the TMario OBJECT — resolvable the same way `gpMarDirector` was, by scanning for the object whose
fields match a known layout rather than by trusting a constant.

## A REAL BUG IN THE VTABLE RECOVERY: nested MWCC names were never parsed, so no JDrama class was

`us_vtables.py`'s `owning_classes` read `Q<k>` qualified names with a plain `<len><ident>` scan:

    perform__Q26JDrama12TPolarCameraF...  ->  {'JDrama12TPolarCameraFUlPQ2'}   (garbage)
    perform__13TMirrorCameraF...          ->  {'TMirrorCamera'}                (correct)

`Q2` introduces two qualified components; the scan took the `2` of `Q2` and the `6` of `6JDrama` as
a single length `26` and produced a 26-character non-name. **Every `JDrama::`-namespaced class was
therefore invisible to the tagger** — including every camera — while non-namespaced classes parsed
correctly and made the tool look healthy. The coverage audit reported 95.9% throughout, because it
uses a different matcher; the two never disagreed out loud.

Fixed (parse `Q<k>` then k `<len><ident>` components, tag on every component):

    TActor vtables              : 694 -> 754   (+60 JDrama-namespaced actors, silently missing)
    TPlacement-but-not-TActor   :   2 ->  17   (cameras and other placements)

The 60 recovered actors were missing from `kTActorVtables` for the whole arc.

### The allowlist root was wrong too, and for a principled reason

The snapshot writes `mPosition` at `+0x10`, which is **`TPlacement`'s** field — so anchoring the
allowlist on `TActor` excluded every TPlacement that is not an actor by construction, and
`JDrama::TCamera : public TPlacement` is exactly that. `us_vtables.py` now takes `--root` and
`--exclude-root`, and the root is chosen from the FIELD being written rather than from whichever
class feels canonical.

Two disjoint lists, because widening is only safe field by field: `mRotation@+0x30` is a TActor
field, and `JDrama::TCamera`'s own layout ends at 0x30 — a rotation write there lands on what the
concrete subclass keeps next, which for `TLookAtCamera` is `mTarget`/`mUp`/`mFovy`, i.e. the
camera's AIM. That failure would look like the camera pointing somewhere else and would be blamed on
the interpolation rather than on the write. So placements get position substituted and rotation
never (`Entry::posOnly`).

### Measured after wiring

    placement-only (position substituted, rotation NOT) = 17,992 dispatches   (~12 per tick)
    movers still 2.7%, largest delta still 885.66 "ゲーム看板8"

Placements are now snapshotted and substituted. The mover share and the largest mover did NOT
change, so whatever drives the frame's 87%-per-tick delta is still not in the substituted set — the
covered placements are not the active camera, or the camera's motion does not live in its
`mPosition`. `TLookAtCamera::perform` builds the view from `mPosition`, `mUp` AND `mTarget`
(`C_MTXLookAt`), and only the first of those three is being substituted, which is a concrete next
thing to check rather than a mystery.

## THE CAMERA IS COVERED — so the remaining gap is its AIM, not its membership

Naming the placement-only objects at runtime (rather than counting them) answers the coverage
question directly:

    PLACEMENT-ONLY objects seen (1):
      0x81588cd0 vptr=0x803acde8 "camera 1"

The gameplay camera IS snapshotted and its `mPosition` IS substituted. So "the camera is excluded by
class", recorded two entries ago, is now only half true: it was excluded before the nested-name fix
and the TPlacement root, and it is not excluded any more.

What is still not substituted is the rest of the camera's pose. `JDrama::TLookAtCamera` keeps

    /* 0x30 */ mUp        <- exactly where TActor keeps mRotation
    /* 0x3C */ mTarget
    /* 0x48 */ mFovy

and `TLookAtCamera::perform` builds the view with `C_MTXLookAt(mViewMtx, &mPosition, &mUp,
&mTarget)` — all three. Interpolating position alone moves the eye but leaves the aim pinned to tick
N, which is not a midpoint of anything. (The 0x30 collision is also why placements are position-only:
a rotation write on this object would land squarely on `mUp`.)

The gameplay camera is `CPolarSubCamera : public JDrama::TLookAtCamera` (decomp
`include/Camera/Camera.hpp:52`), so the field offsets above apply to it directly.

### An open question the next session must answer FIRST, before adding fields

`TSmJ3DScn::perform` takes the view from `param_2->mViewMtx` — the TGraphics the caller passes — and
the sub-frame passes a SNAPSHOT of that struct taken during the tick's draw block. So a camera whose
`perform` recomputes the view during the re-issue only reaches the scene if it writes into the
buffer we pass AND runs before `TSmJ3DScn` in the re-issued order. Whether the camera's perform is
in the re-issued draw lists at all has not been established — the tick trace shows a `0x10`
(projection) dispatch inside the draw block, which is suggestive and not proof.

Adding `mUp`/`mTarget` to the snapshot before settling that would produce another "substituted
correctly, changed nothing" result, and this arc has enough of those.

## Two report lines were lying, and are fixed

* `re-issue set: 0 lists` — it printed `g_drawN` AFTER the sub-frame clears it, so it always read
  zero regardless of how many lists were re-issued. Now keeps the last real value.
* The `COVERS` line hardcoded "694 entries" and named only `kTActorVtables`, so it kept asserting a
  coverage claim that the vtable fix had already changed (754), and never mentioned the
  placement list or the camera-aim gap. Now derived from the arrays and explicit about what it
  does not cover.

A diagnostic that states its own coverage from a hardcoded number is a stale claim with a timestamp
of whenever someone last edited the string.

## THE CAMERA'S AIM IS NOW SUBSTITUTED — both preconditions measured first

Before adding any field, the two things that had to be true were measured:

    view across re-issue #1: 6 of 12 elements changed (max |d|=1.0000),
                             camera dispatched 16x inside the sub-frame

The camera's `perform` DOES run inside the re-issue, and it DOES write the `TGraphics` the sub-frame
passes (the snapshot buffer, not the long-gone stack original). Had the view been byte-identical
across the re-issue, no camera field could have reached the sub-frame and adding `mUp`/`mTarget`
would have produced yet another substituted-correctly-changed-nothing result.

### Identifying the camera: by LAYOUT, because the vtable route provably fails here

"camera 1" carries vptr `0x803acde8`; the recovered TLookAtCamera tag sits at `0x803ace0c`, exactly
`0x24` later. That is the SECONDARY vtable — `class TCamera : public TPlacement, public
JStage::TCamera`, with the `JSG*` methods that carry the tag in the second table while the live
object's `+0x00` points at the primary. The same multiple-inheritance trap already documented for
TActor in `us_vtables.py`. Deriving the primary from the secondary by that `0x24` would be a magic
constant, so the object is identified by its FIELDS instead:

    near=10.00  far=300000.00  fovy=50.00  aspect=1.346
    up=(0.0, 1.0, 0.0)  target=(-553.9, 459.9, 6282.8)   -> looks_like_lookat_camera = YES

`near`/`far` are not merely plausible, they are the EXACT constants `TLookAtCamera`'s constructor
passes (`TCamera(10.0f, 300000.0, name)`, JDRCamera.hpp:73), `up` is a canonical up vector and
`target` a real world-space point. Two exact constants plus two range checks is a falsifiable
signature, which matters because what it authorises is writing to `mUp@+0x30` and `mTarget@+0x3C` —
on a non-camera, someone else's data.

### What is substituted now

`Entry` carries `up`/`target` prev and cur for layout-verified cameras, lerped linearly alongside
the eye. Deliberately NOT a slerp: `mUp` is a world-space up vector and `mTarget` a look-at POINT,
not an orientation, so the shortest-arc problem that applies to Euler rotation does not arise.

Endpoints stay exact and the restore puts all six components back, so the camera's own tick-N pose
is what physics and the next tick see.

## WORKFLOW DEFECT: every diagnostic run in this arc idled for minutes after finishing

The user asked what the point of the multi-minute runs was. There wasn't one.

Two separate mistakes:

1. **`SB_DUMP_FRAME_AFTER=1500` was inherited, not derived.** It dates from the title/file-select
   work, where the menus had to be driven to reach the interesting state. Under `SBR_FASTBOOT=1` the
   plaza is live within a couple of hundred ticks and the pad waypoints (400/1400/2200) were never
   re-derived. Most of this session's measurements — apply-time coverage, the view-matrix delta,
   camera identity, sub-frame counts — print to the log and need no frame dump at all.

2. **Nothing stopped the process once it had produced what it was started for.** Run length was set
   by whatever `timeout` value the caller guessed, so a 300-present dump still burned 200 seconds of
   wall clock idling past the point of interest.

`SBR_QUIT_AFTER=<presents>` fixes (2): the run shuts down cleanly once it reaches the given present.
The margin above the dump index matters — the dump's texture->buffer copy is mapped on the NEXT
present, so quitting exactly at the dump truncates the file the run existed to produce.

    before : SB_DUMP_FRAME_AFTER=300, no quit  -> 200 s
    after  : ... SBR_QUIT_AFTER=320            ->   9 s

**22x**, on every diagnostic run. Roughly two dozen runs in this arc paid the old cost, which is
most of an hour spent watching an idle process. A tool that cannot stop when it is done is a
workflow defect, and it outranked the task in hand.

## ROOT CAUSE FOUND: the sub-frame was never COPIED OUT — and the frame seam presents BEFORE the copy

The sub-frame emitted 1.7 MB of geometry and produced a frame pixel-identical to the main frame in
every configuration tried. The reason, measured directly:

    copy-to-XFB triggers emitted by this sub-frame: 0

A pass that renders into the EFB and never copies it out is invisible, however much it draws. The
display simply keeps showing the last copied XFB. That single fact explains every `main -> sub = 0`
in this arc, and it explains why `SBR_INTERP60_DROPLAST` moved the MAIN frames but never that zero
(EFB state leaking into the next tick's copy).

`GXCopyDisp` lives in `JDrama::IssueGXCopyDisp`, called from `TDisplay::endRendering`
(JDRDisplay.cpp:36). Calling the game's own `endRendering` for the sub-frame is the faithful way to
emit it — the copy's src rect, Y scale, dst width, clamp, filter and clear all come from the
display's render mode, and hand-rolling that sequence would be a second copy of it to keep in step.

### And it exposed a deeper ordering problem that predates all of this

With the copy in, the sub-frames carry the image and the MAIN presents come back BLACK:

    copy.rgba.0.sub   mean RGB = ( 65.8,  77.9,  65.2)
    copy.rgba.1.main  mean RGB = (  0.0,   0.0,   0.0)

`TDisplay::endRendering` calls `waitForRetrace` FIRST and `IssueGXCopyDisp` SECOND. The frame seam
IS `waitForRetrace` — so **the main present has always happened before the tick's own EFB->XFB
copy.** It only ever looked correct because the display kept showing the previously copied XFB, one
frame stale. Adding a copy inside the sub-frame (which also clears the EFB) removed the thing that
was hiding it.

That is a property of the frame seam itself, not of interpolation, and it means the recomp has been
presenting a frame-late image all along — worth checking against the decomp runtime, whose seam is
the same function.

### Left behind a switch, deliberately

`SBR_INTERP60_COPY=1` is OFF by default. It is the right mechanism and the wrong placement: the
presents need to move to after the copy, which is a change to the frame seam, and that is not a
change to make blind at the end of a long session. Default path re-verified as rendering normally
(both presents mean R ~65.8, not black).

## PRESENT-AFTER-COPY WORKS — and the last link is that the CAMERA's prev == cur

`SBR_PRESENT_AFTER_COPY=1` defers the present to the end of `TDisplay::endRendering`, after
`IssueGXCopyDisp`. `waitForRetrace` has exactly one call site (JDRDisplay.cpp:38), so this is a
single-path change, not a guess about other callers. With it, plus `SBR_INTERP60_COPY=1`:

    pac.rgba.0.sub   mean R = 65.8      no black frames — both presents render
    pac.rgba.1.main  mean R = 65.8
    sub  -> main :        0
    main -> sub  :  895,413 (72.9%)     the sub-frame is VISIBLE and distinct at last

Two arc-long defects are fixed together: the sub-frame now copies itself out of the EFB, and the
main present now happens after the copy rather than one frame stale.

But the sub-frame equals the FOLLOWING main frame, i.e. it renders pose N, not a midpoint. The
reason is measured and specific:

    CAMERA prev->cur #1: eye moved 0.000, target moved 0.000

**The camera's snapshot captures its pose after its own update, so prev == cur exactly.** Every
alpha then produces the same view, which is pose N. The lerp is correct and has nothing to work
with — the same shape of defect as the coverage gaps, one level further in: not "is the object in
the set" but "is the sampled moment the right one".

### Next, and the measurement that settles it

Where in the tick does the camera's `mPosition` actually change? The snapshot fires on the first
`CUE_MOVE`-bearing dispatch of a tick, which for the camera is in the draw block; if the camera is
updated earlier in the window than that, the captured "prev" is already the new pose. The existing
`SBR_INTERP60_FOLLOW=<addr>` follower prints a chosen object's position at every phase transition
and will show it directly — the camera's address is printed by the placement-only report
(`0x81588cd0` in the current build), so no new instrument is needed.

Do NOT fix this by moving the snapshot point on reasoning. Three placements of an apply/restore
bracket were derived that way earlier in this arc and all three were wrong; the phase order is
cheap to observe and expensive to guess, and now costs 9 seconds to observe.

## RETRACTED: "the camera snapshot captures the pose after its own update"

That was concluded from four `eye moved 0.000, target moved 0.000` samples. Running longer:

    CAMERA prev->cur #300: eye moved 29.459, target moved 29.557
    CAMERA prev->cur #600: eye moved 21.686, target moved 27.125

The camera moves and IS captured. The zeros were from before gameplay begins (`mAppState -> 5
(GAMEPLAY)` arrives later); the snapshot point is fine. The diagnostic's own wording caused the
error — it printed a CAUSE ("captured the pose AFTER its own update") next to a measurement that
only supported "these two values are equal right now". Reworded to state what it measured.

**This also invalidated the fast test moment.** Dumping at present 300 to make runs quick moved the
measurement to a pre-gameplay moment with a static camera — the speed fix silently changed what was
being measured. Live-moment runs now use a later dump with the pad script starting at 600.

## THE SUBSTITUTION SURVIVES — the scene takes its view from the SNAPSHOTTED TGraphics

    camera pose after re-issue #600: (-0.00, _, 7979.34)
      lerp wrote (-0.00, _, 7979.34)   cur is (-0.00, _, 7968.49)

The lerped pose is in place during the whole re-issue and is NOT recomputed away by the camera's own
perform. So every link now works:

    sub-frame renders from its own stream        1.7 MB, measured
    it copies itself out of the EFB              SBR_INTERP60_COPY, triggers 0 -> 1
    the present happens after the copy           SBR_PRESENT_AFTER_COPY, no black frames
    the camera is snapshotted and moves          ~22-29 units/tick
    the lerp is written and survives             measured above

and yet, at a live moment with the camera moving:

    sub  -> main :         0
    main -> sub  : 1,094,666 (89.1%)

The sub-frame is visible and distinct, but identical to the FOLLOWING main frame — pose N, not the
midpoint.

### The remaining link, stated precisely

`TSmJ3DScn::perform` takes the view from `param_2->mViewMtx` — the TGraphics passed IN — and the
sub-frame passes a SNAPSHOT taken during this tick's draw block, which already holds the view built
for pose N. The camera's perform does write that buffer during the re-issue (6 of 12 elements
change), but the scene has its own list position: if the scene's list is re-issued BEFORE the list
holding the camera, the scene copies the stale view and the camera's recomputed one arrives too
late.

So the next step is an ORDERING question inside the re-issue, and it is measurable rather than
arguable: record which re-issued list the camera is dispatched from and where the scene's
`TSmJ3DScn` sits relative to it, then re-issue the camera's list first (or run the camera's perform
explicitly before the draw lists, which is what `TLookAtCamera::perform` exists to do).

Do not "fix" it by writing `mViewMtx` into the snapshot directly. That would substitute a matrix
rather than a pose — the matrix-lerp approach the standing directive rejects — and would leave
everything that reads the camera's FIELDS (shadow projection, LOD, culling) on the un-interpolated
value.

## FIVE CAMERAS WRITE THE VIEW PER SUB-FRAME — and the one we substitute is not driving it

Attributing every `mViewMtx` write inside a sub-frame to the dispatch that made it (names decoded
from Shift-JIS):

    #1 0x816533ec <TOrthoProj>          1.07 -> 0.00
    #2 0x810e52b4 鏡カメラ (mirror)      0.00 -> -4942.61      <- sets the 3D view
    #3 0x81588cd0 camera 1          -4942.61 -> -4942.61      <- the object we substitute; NO CHANGE
    #4 0x810a5b28 ブラーカメラ (blur)   -4942.61 -> 0.00
    #5 0x81588cd0 camera 1              0.00 -> -4942.61
    #6 0x81661aa8 <TOrthoProj>      -4942.61 -> 0.00
    #7 0x81588cd0 camera 1              0.00 -> -4942.61
    #8 0x81661c44 <TOrthoProj>      -4942.61 -> 0.00

The view is written eight times per sub-frame by five distinct objects, with ortho passes zeroing it
between 3D ones. `camera 1` — the only object `looks_like_lookat_camera` accepts, and the one whose
`mPosition`/`mUp`/`mTarget` are lerped — DOES write the view, and writes **-4942.61 at alpha 0.0,
0.5 and 1.0 alike**, and under a 5000-unit kick as well.

So its view output does not depend on the `mPosition` being substituted. Either the field driving
`C_MTXLookAt` is not the one at `+0x10` for this class, or its `perform` recomputes the pose from
internal state (a polar camera derives its position from angle/distance/target chase, which would
make `mPosition` an OUTPUT of the update rather than an input to it).

### Two instrument defects this exposed, both the same shape

* **The attributor latched on the FIRST writer** and named 鏡カメラ — the reflection pre-render,
  which writes a view several dispatches before the scene's. "Who writes a view" and "who writes the
  view the scene uses" differ by four dispatches. Cap the boring case, not the interesting one; it
  now reports every writer in order.
* **`SBR_PAD_SCRIPT` keys on PAD READS (one per tick) while `SBR_QUIT_AFTER` and the dump key on
  PRESENTS**, and the sub-frame makes presents run at 2x ticks. Waypoints at ticks 600/900 were
  therefore never reached in a run that quit at present 1320 (tick 660) — every "live moment" run
  before this one was measuring a stationary camera. Two instruments counting different populations,
  joined by a number that looks shared and is not: the exact trap CLAUDE.md lists.

### The next question, precisely

What does `camera 1`'s `perform` actually read to build its view? That is answerable from the
disassembly of its `perform` slot (the object's primary vtable is at `0x803acde8`) rather than by
more runtime bisection — read which offsets it loads before `C_MTXLookAt`. If it derives the pose
from polar state, the game-native interpolation has to snapshot THAT state, not the derived
position, and the same will be true of every actor whose transform is recomputed rather than stored.

## RE RESULT: the camera's view is a CACHED MATRIX, and the game already keeps prev/cur for its pose

`camera 1` is `CPolarSubCamera`. Its `perform` is vtable idx6 = `0x80023004` (slot index established
from two independently NAMED cases — `TMirrorCamera::perform` and `TOrthoProj::perform` both sit at
idx6 — rather than counted off the class hierarchy). Disassembled, it has two disjoint paths:

    cue & 0x1  (movement)
        0x124 -> 0x13c,  0x148 -> 0x160,  0x16c..0x1a8 -> 0x1ac..0x1e8      <- SAVE PREVIOUS
        bl ctrlOptionCamera___15CPolarSubCameraFv
        bl ctrlGameCamera___15CPolarSubCameraFv
        bl calcFinalPosAndAt___15CPolarSubCameraFv                          <- compute the new pose
        ... stfs 0x30/0x34/0x38(r29)                                        <- mUp written from a matrix

    cue & 0x14 (view / projection)
        0x16c..0x1a8 -> TGraphics+0x74     (mProjMtx)
        PSMTXCopy(0x1ec -> TGraphics+0xb4) (mViewMtx)

**The view path computes nothing.** It copies a matrix cached at `+0x1ec`, built during the movement
phase. `mPosition@+0x10` is not an input to it — which is the complete explanation for every null
result: the substitution was correct, survived the re-issue, and was read by nobody, because
`C_MTXLookAt` never runs at view time for this class. A 5000-unit kick left the view byte-identical
for the same reason.

### And the game keeps its own prev/cur — measured

    +0x124 = (4267.43, 615.32, 4935.66)   prev@+0x13C = (4267.43, 615.32, 4932.72)
    +0x148 = (4950.01, 463.04, 4910.52)   prev@+0x160 = (4950.01, 463.04, 4907.58)
    mTarget@+0x3C = (4950.0, 463.0, 4910.5)

`+0x148` IS the target (`mTarget` is a copy of it) and `+0x124` is the eye. Both prev slots trail by
exactly **2.94** in z — the same per-tick camera motion the `CAMERA prev->cur` probe measured
independently (2.938). The game snapshots its own camera pose every tick, for its own reasons.

This is a better interpolation source than anything external: no snapshot, no vtable allowlist, no
layout signature, and no risk of sampling at the wrong moment — the prev/cur pair is maintained by
the code that owns the fields.

### Next

The sub-frame needs the view matrix at `+0x1ec` rebuilt from `lerp(prev, cur)` of `+0x124`/`+0x148`.
Two candidates, and the choice should be settled by reading rather than by preference:

* find the guest routine that builds `+0x1ec` from the pose (inside or after
  `calcFinalPosAndAt___15CPolarSubCameraFv`) and call it with lerped inputs — fully game-native; or
* build it with the game's own `C_MTXLookAt` from lerped eye/target/up and write `+0x1ec` for the
  duration of the sub-frame — uses the game's inputs and math but duplicates one build step.

Neither is "matrix lerping": both derive the matrix from an interpolated POSE, which is what the
standing directive asks for. Blending `+0x1ec` between its cached and saved copies WOULD be matrix
lerping and is the thing to avoid.

## THE CAMERA INTERPOLATES — exactly, in the game's own terms

The sub-frame now does what `CPolarSubCamera::perform` does, with an interpolated pose: lerp the
game's OWN prev/cur (`0x13c`->`0x124`, `0x160`->`0x148`) and call the game's OWN
`C_MTXLookAt(0x1ec, 0x124, 0x30, 0x148)` with the game's own argument list, then let the view path
copy `0x1ec` as it always does.

Measured view translation, same tick, three alphas:

    alpha = 0.0 : -4939.68      (and the NEXT sample reads -4942.61)
    alpha = 0.5 : -4941.14      midpoint of -4939.68 and -4942.61 = -4941.145
    alpha = 1.0 : -4942.61

**Exact to the printed precision**, and `alpha=0.0` reproduces the previous tick's view. This is
interpolation working, in the form the standing directive asks for: the matrix is DERIVED from an
interpolated pose by the game's own builder, never blended between two cached matrices.

Identification is an address equality, not a heuristic: the object's vtable slot 6 must BE
`0x80023004`, the very function whose disassembly these offsets came from. The earlier
near/far/fovy signature had already accepted an object whose pose was not where it assumed.

### What is still wrong, and it is no longer the interpolation

    main -> sub : 676,892 (55.1%)
    sub  -> main:       0

The sub-frame is visibly distinct from the main frame before it, but still pixel-identical to the
main frame AFTER it — while the view it rendered with is provably a midpoint of the two. Those two
facts cannot both describe the presented image, so the discrepancy is in the PRESENT path, not in
the pose or the matrix: something is showing the following tick's stream where the sub-frame's
should be.

That is a much narrower question than any asked so far, and both halves are now instrumented
(`SBR_INTERP60_ORDER` prints the view actually used; the labelled dump series says which present is
which). The next measurement is which stream each present actually renders — the sub-frame's 1.7 MB
or the next tick's — rather than which image it resembles.

## THE STREAM ORDERING BUG IS FIXED — but alpha still does not reach the PIXELS

Under `SBR_PRESENT_AFTER_COPY`, `gxfifo_build()` and `gxfifo_send_last()` were still running inside
the seam, i.e. BEFORE the game's `GXCopyDisp` — so each tick's stream was closed and sent without
its copy, and the copy landed in the NEXT tick's stream. Moving both into `present_tail()` fixes it,
and the symptom moved: `sub -> main` went from exactly 0 to ~3,800 px.

Then an alpha sweep, same tick, same everything:

    alpha=0.00 :  sub->main =  3,774    main->sub = 678,914
    alpha=0.25 :  sub->main =  3,778    main->sub = 678,928
    alpha=0.50 :  sub->main =  3,782    main->sub = 678,940
    alpha=0.75 :  sub->main =  3,772    main->sub = 678,946
    alpha=1.00 :  sub->main =  3,771    main->sub = 678,955

**Flat.** 0.08% and 0.006% variation across the full range, while the view matrix the sub-frame
renders with is a verified exact midpoint that moves 2.94 units end to end. The interpolation is
correct and is not reaching the presented image.

So the open question is now entirely inside the present/copy path: the sub-frame renders with an
interpolated view (proven), emits its own 1.7 MB stream (proven), issues its own copy-to-XFB
(proven, triggers 0 -> 1) — and the image that reaches the dump does not vary with any of it. The
remaining candidates are that the sub-frame's copy targets a buffer that is not what the following
present reads, or that aurora's present source lags a frame.

### A labelling slip in the scoring script, recorded because it nearly repeated the earlier mistake

The sweep script printed columns headed `prev_main->sub` and `sub->next_main`, but with
`SB_DUMP_FRAME_COUNT=3` the series is sub/main/sub — so those were `sub->main` and `main->sub`. The
conclusion survives (both are flat), but the header was wrong for the same reason the earlier
readings were: a pairing assumed rather than read from the runtime labels that are right there in
the filenames. Ad-hoc scoring scripts must use `role_of()` like `subframe_gate.py` does, not
positional assumptions.

## THE PIPELINE IS CONNECTED END TO END — cross-run diff at a fixed present index

The flat sweep compared adjacent presents WITHIN a run, which cannot separate "alpha does nothing"
from "alpha moves both neighbours together". Comparing the SAME present index ACROSS runs
(deterministic, so this is valid) answers it directly:

    alpha=0.0 vs alpha=1.0, same present index:
      present 0 [ sub] :  351 px differ
      present 1 [main] :    0 px differ      <- correct: a main frame must not depend on alpha
      present 2 [ sub] :  373 px differ

The sub-frame DOES respond to alpha and the main frame correctly does not. Every stage is therefore
connected: interpolated pose -> game's own C_MTXLookAt -> cached view matrix -> view path ->
sub-frame stream -> its own copy -> present -> dump.

**But only ~350 of 1,228,800 pixels respond**, while the view moves 2.94 units end to end — which at
this camera should move a large fraction of the screen. So most of the scene is NOT being drawn
through the view camera 1 writes.

### The likely reason, and the measurement that would settle it

The view-writer census showed `mViewMtx` written EIGHT times per sub-frame by FIVE objects, in this
order: an ortho pass, 鏡カメラ (mirror), camera 1, ブラーカメラ (blur), camera 1, ortho, camera 1,
ortho. In the uninterpolated run camera 1's writes were no-ops because it produced the same matrix
the mirror camera had just written — they agreed. With camera 1 now writing a MIDPOINT they no
longer agree, and any draw that consumes the mirror camera's value (or a later ortho zero) gets the
endpoint regardless.

So the question is which of those eight writes is live at the moment `TSmJ3DScn::perform` copies
`param_2->mViewMtx` into j3dSys — not which object writes last overall. That is one more attribution
probe of the shape already built (watch the value at the scene's own dispatch rather than at list
boundaries), and it should be written to report EVERY consumer, not the first, for the reason the
previous attributor had to be fixed.

Do not respond by interpolating the mirror/blur cameras too until that measurement exists: they are
pre-render passes for reflection and motion blur, and matching their views to the sub-frame may be
correct or may be exactly wrong, depending on what each pass is for.

## ANSWERED: the first 3D pass of a sub-frame renders through the MIRROR CAMERA's cached view

Watching the DESTINATION (`j3dSys`'s view matrix at `0x804045DC`) rather than the source names the
consumer directly. Per sub-frame it is set four times, all by a `J3D System Set View Mtx` node:

    -4942.61 -> -4942.61     endpoint  (the value 鏡カメラ wrote from ITS cached matrix)
    -4942.61 -> -4941.14     <- our interpolated midpoint
    -4941.14 ->     0.00     ortho pass
        0.00 -> -4941.14     <- midpoint again

So the interpolation reaches `j3dSys` — for the second and fourth passes. The FIRST pass, which is
the largest, renders through the endpoint view the mirror camera had cached during the movement
phase. That is the whole explanation for "only ~350 of 1,228,800 pixels respond to alpha": most of
the frame is drawn by the pass that never sees the interpolated view.

This is the measurement the previous entry said had to exist before touching the mirror camera. It
now does, and it says the mirror camera is not incidental — it supplies the view for the dominant
pass.

### Next, and what to check before assuming

`TMirrorCamera` (US `perform` at `0x80193fbc`, already natively overridden for widescreen) derives a
reflected view from the main camera and caches it during movement, exactly as CPolarSubCamera does.
The same treatment should apply: rebuild its cached matrix from the interpolated main-camera pose
for the duration of a sub-frame.

Two things to establish first, by reading its perform rather than by trying it:

* WHICH pass the mirror camera's view is for. If the first `j3dSys` set is the reflection
  pre-render (drawing the scene into a reflection texture), then interpolating it is right. If it is
  the main scene pass and the mirror camera merely happens to write the view, the naming is
  misleading and the fix belongs elsewhere.
* Whether its cached matrix is derived from the main camera's POSE or from the main camera's
  MATRIX. If the latter, rebuilding it from an interpolated pose needs the main camera's matrix
  interpolated first — which is available, since that is what `camera_apply` already builds.

## FALSIFIED: the mirror camera does NOT supply the dominant pass. The camera is EXACT.

The previous entry's conclusion — "the first and largest 3D pass renders through the mirror
camera's cached view, and that is the entire explanation for only ~350 of 1,228,800 px responding
to alpha" — is **wrong in both halves**, and it was wrong for the reason this project keeps paying
for: it JOINED TWO INSTRUMENTS. The view-writer census (one probe, one run) said the write before
the scene's first copy came from 鏡カメラ; the j3dSys watch (another probe, another run) said the
first copy carried the endpoint value. Neither observed the other's events, so the ordering the
conclusion rests on was never seen by anything.

Both probes also compared element [3] of the view matrix alone — its row-0 translation. A
reflection through a near-horizontal plane leaves the right vector horizontal and the eye's x/z
unchanged, so element [3] of a REFLECTED view is nearly identical to the main camera's. The one
number being read could not distinguish the two hypotheses it was used to choose between.

### One instrument: `SBR_INTERP60_VIEWSEQ=1`

Every `gfx->mViewMtx` write and every `j3dSys` view change of ONE sub-frame, interleaved in
dispatch order, with the GX bytes each pass emits and row 1 (the up axis, where a reflection
inverts) printed for both sides:

    [23] GFX  view <- t=(-122.22,-1176.86,-5546.17) up=(-0.004,0.978, 0.209)  "鏡カメラ"
    [24] SCENE view <- (pass 1 begins)                                        59 KB
    [59] GFX  view <- t=(-122.22,  590.06,-5671.33) up=( 0.004,0.978,-0.209)  "camera 1"
    [60] SCENE view <- (pass 2 begins)                                      1542 KB   <- the frame
    [623] SCENE (ortho, pass 3)                                                4 KB
    [630] SCENE (pass 4, camera 1's view again)                               65 KB

Pass 1 IS the mirror camera and its view IS a genuine reflection (up.z inverted, eye mirrored
about y) — but it is 59 KB of 1670, a reflection pre-render, not the frame. The dominant pass is
**camera 1's, and it carries the interpolated view**: t.x is -122.22 at alpha=1.0 and -66.17 at
alpha=0.0 in the same sub-frame.

### The arming bug that produced the original reading, and the fix

The first two runs of this probe printed IDENTICAL views at every alpha, at presents 1100 and 1600
— and 1100 and 1600 printed the same view as each other. The camera was PARKED at both. A present
index is not a control: `camera_apply` reports `|cur-prev| = 0.000` at exactly those moments. The
probe now arms on camera motion (`SBR_INTERP60_VIEWSEQ_MIN`, default 2.0 units) and prints the
separation in its header, so the number any alpha difference must come out of is on the same line
as the result. `camera_apply` no longer returns a silent empty CamSave either — it says so, with
the vtable slot it rejected.

### What the pixel numbers actually say — with MAGNITUDE, not just counts

Consecutive presents of ONE run, roles stamped by the runtime, plus mean |delta| per channel
because a differing-pixel COUNT has no magnitude ("75.5% differ" fits both a one-tick shift and a
1-LSB difference everywhere):

    pair                                  differing px   mean |d|/channel
    main(1) -> main(3)  [one full tick]        947,275             11.070
    sub@alpha=1 vs main(3)  [identity]           3,116              0.075
    sub@alpha=0 vs main(1)  [should be ~0]     927,987              9.043
    sub@alpha=0 vs main(3)                     206,636              3.124

Main frames are byte-identical across alphas (0 px) — alpha must not reach them, and it does not.
alpha=1.0 reproduces the following main frame to 0.075/channel: **the sub-frame pipeline is
correct end to end.** But alpha=0.0 lands only ~28% of the way back toward the preceding main
frame instead of ON it.

### And it is NOT the camera. `SBR_INTERP60_CAMTRACE=1`

The cached view matrix's translation, before and after the substitution, one line per tick, at
alpha=0.0:

    present 2794: BEFORE=(-37.17,590.30,-5672.47)  AFTER=(-17.91,590.32,-5672.56)
    present 2796: BEFORE=(-66.17,590.25,-5672.22)  AFTER=(-37.17,590.30,-5672.47)
    present 2798: BEFORE=(-122.22,590.06,-5671.33) AFTER=(-66.17,590.25,-5672.22)

AFTER on tick N equals BEFORE on tick N-1 **exactly, to every printed digit** — the camera
substitution reproduces the previous tick's view precisely. At alpha=1.0, AFTER == BEFORE on every
line, exactly. Both directions of the camera are verified, from the camera's own previous frame
rather than against an expectation.

So the camera moves a FULL tick between alphas while the image moves ~28% of one. The remaining
~72% is held by a population that is neither the camera nor a snapshotted TActor transform. The
standing suspects, in the order they should be ablated:

* **animation phase.** The sub-frame re-runs cue 0x2, which advances every BCK/BTK a second time;
  at alpha=0 the transforms are rewound but the skeletons and texture scrolls are not — this is the
  already-recorded residual whose proper fix is suppressing the frame ADVANCE, not the phase.
* **the mirror and blur pre-renders**, measured above as NOT interpolated (identical across alphas).
  59 KB and a blur camera — small, and now quantified rather than assumed.
* **JPA particles**, which carry no snapshotted transform at all.

The next measurement is an ABLATION with separate alphas for camera and actors, so the deficit is
attributed to a population rather than argued from a list. Do not fix any of the three above until
that split exists: the previous entry is what guessing between them costs.

## THE ABLATION: the camera is the ONLY thing being interpolated. Actors reach zero pixels.

`SBR_INTERP60_ALPHA_CAM` / `SBR_INTERP60_ALPHA_ACT` drive the two populations independently (both
default to `SBR_INTERP60_ALPHA`, so the split cannot change an unsplit run). Its control — cam and
act both 0.0 must reproduce the single-alpha 0.0 result — passes exactly. Scored against the
neighbouring main frames of the same run, at the present the camera-motion arming picked:

    configuration          vs main(prev)          vs main(next)
    reference: one tick    947,275 / 11.070
    both alpha=1             947,587 / 11.120       3,116 /  0.075
    both alpha=0             927,987 /  9.043     206,636 /  3.124
    split ctl cam0 act0      927,987 /  9.043     206,636 /  3.124   <- control passes
    cam=0 act=1              927,987 /  9.043     206,636 /  3.124   <- act makes NO difference
    cam=1 act=0              947,587 / 11.120       3,116 /  0.075   <- act makes NO difference

`cam=0 act=1` and `cam=0 act=0` are **byte-identical** (`cmp` says so, not a count). Every pixel the
sub-frame moves, the camera moves. The actor substitution moves none.

And it is not idle. Per sub-frame (`SBR_INTERP60_ACTTALLY=1`): **400 entries substituted, 8 with
prev != cur, largest ~20 units**, one of them マリオ. The pose reaches guest memory — the read-back
check reports `stuck=118400 lost=0` — and does not reach a pixel.

### Two hypotheses tested and both falsified

**The phase boundary.** An actor's pose becomes geometry in `TLiveActor::perform`'s 0x2 branch
(`calcRootMatrix` + `MActor::calc`), dispatched by the director's CALC-ANIM list at +0x2C — not by
the draw lists, which the actor is registered in with the draw cue. The sub-frame re-issues only the
draw block, so the obvious cause was that nothing recomputes a root matrix from the substituted
pose. `SBR_INTERP60_CALCANIM=1` re-issues that list. The actor alpha still moves **0 pixels**, and
the frame gets far worse — mean |d| 21.9 against both neighbours, versus 0.075 for the clean
identity — exactly the double-advanced-animation residual this file predicted. Cause not found;
cost confirmed.

**The kick.** `SBR_INTERP60_KICK=3000` displaces every substituted entry by 3000 units. Against an
unkicked sub-frame at the same alpha: **0 of 1,228,800 pixels differ.** The codemap's recorded
"reach 97.16% under a 3000-unit kick" does not reproduce at HEAD and is corrected there. This is the
cleanest statement of the defect: a 3000-unit displacement applied to 400 objects, written and
read back successfully, changes nothing on screen.

### What the player-coverage probe says, and why it is a lead rather than an answer

    PLAYER COVERAGE: gpMarioOriginal -> 0x8136384c "<unreadable>" vptr=0x00000000
      is_tactor=NO in_snapshot_table=NO
      the player's vtable is NOT in kTActorVtables -- he is never snapshotted and never
      substituted, so no alpha can move him

`vptr=0x00000000` at that address means the probe is reading something that is not an object, so its
verdict on the player is not yet trustworthy — it may be reading `gpMarioOriginal` wrongly rather
than reporting a real exclusion. But it points where the next measurement goes: **which objects
are in the 400, and are any of them the ones on screen?** A substitution that reaches memory,
survives read-back, and changes no pixel is most simply explained by substituting 400 objects that
nothing draws.

The claim is recorded as C027 with the kick as its falsifier. The camera-side result stands on its
own and is unaffected: `SBR_INTERP60` today is a working CAMERA interpolator, and that is what the
codemap now says.

## The census, and where the actor path actually breaks

`SBR_INTERP60_ACTCENSUS=1` names the COMPLETE substitution set of one sub-frame (400 objects, not a
sample). Counted by name:

    82  コイン（モデルなし）   coin, NO MODEL      20  コイン        19  水ヒットコイン
    18  palm      13  manhole      12  青コイン      11  鳥      10  ジュースブロック
    10  ゲーム木箱      10  エフェクト水柱       9  やしの実（無限）  ...

It is Delfino's scenery: coins (82 of them explicitly modelless), manholes, palms, fruit, crates,
birds. Almost all report `moved 0.000`. The eight that move are the NPCs and the player:

    モンテＥ   20.000      キノピオＢ  6.000      モンテＪ   4.800      モンテＣ  4.799
    モンテＩ    1.080      マリオ      2.421      キノピオＡ 0.200      camera 1  0.235

**マリオ at 0x8136383c IS in the set and IS substituted.** The player-coverage probe's verdict —
"the player's vtable is NOT in kTActorVtables, he is never snapshotted" — is WRONG, and its own
`vptr=0x00000000` said so: `gpMarioOriginal` holds 0x8136384c, which is the player object **+0x10**,
i.e. it points at `mPosition` itself, not at the object. The probe dereferenced a float as a vptr.
That line must be fixed before it is believed again; it is exactly the shape of diagnostic that
reports a confident negative its method could never have contradicted.

### The kick, in all four cells

`calcRootMatrix` reads `mPosition`/`mRotation` (decomp `liveactor.cpp:259`) and is called ONLY from
`TLiveActor::perform`'s 0x2 branch, which the director dispatches from its calc-anim list. So the
kick and the calc-anim re-issue had to be crossed, and were not:

    kick=3000, no calc-anim re-issue :      0 of 1,228,800 px   (0.00%)
    kick=3000, calc-anim re-issued   :  2,477 of 1,228,800 px   (0.20%)

So the boundary IS real — re-issuing calc-anim opens the path, and the earlier "calc-anim changes
nothing" reading was measuring the alpha pair, which has its own reason to be flat. But 0.2% is not
reach. A 3000-unit displacement of 400 objects including the player should evacuate a large part of
the screen; it moves what looks like a couple of on-screen props.

Two things follow, and neither is yet measured:

* Most of the 400 are not drawn — 82 are named "coin, no model", and the静 props report zero motion
  because they have none. The substitution set is scenery, and the set that MATTERS on screen (the
  player, his effects, the NPCs near the camera) is a handful of objects inside it.
* **The player's draw does not follow his substituted `mPosition` even with calc-anim re-issued.**
  Kicking マリオ 3000 units should remove him from the frame. That is the next thing to measure, and
  the likely reason is that `TMario` does not reach `TLiveActor::calcRootMatrix` on that cue — the
  player is drawn through his own path, and the seam for him is not the one that works for a crate.

The camera result is unaffected by any of this and remains the one solid piece: `SBR_INTERP60`
interpolates the camera exactly, in both directions, via the game's own prev/cur and C_MTXLookAt.

## FIXED: the player interpolates — `SBR_INTERP60_PLAYER=1`

### Root cause, from the decomp rather than from a pixel diff

`TMario` never reaches `TLiveActor::perform`'s 0x2 branch, so no amount of re-issuing the director's
calc-anim list could ever have moved him — the previous entry's 0.2% reach was other props. His pose
becomes a matrix in `TMario::calcAnim` → `calcBaseMtx`, which builds a TR matrix from `mPosition` +
`mModelFaceAngle` and copies it into the model's base TR matrix (`src/Player/MarioDraw.cpp:1920`
and `:1582`). And `TMario::perform` calls `calcAnim` in exactly one place
(`src/Player/MarioMain.cpp:139`):

    if ((param_1 & 1) && mFreezeTimer <= 0) { thinkAloha(); calcAnim(2, graphics); ... }

**Gated on cue bit 0x1 — the MOVEMENT bit, the one bit a sub-frame must never set**, because the
same branch runs `playerControl()`, the physics. The player's pose-to-matrix step is structurally
unreachable from a draw-only re-issue.

### The fix takes the camera's shape exactly

Call the GAME'S OWN function with an interpolated pose already written, and let it produce the
matrix — `TMario::calcAnim(2, gfx)` (US `0x80244800`) after `apply_all`, the same way `camera_apply`
calls the game's own `C_MTXLookAt`. No lerped matrix, no reimplemented math, cue 2 so the skeleton
recomputes without a frame advance.

### Measured, with the identity control that makes it safe

`calcAnim` also runs callbacks and the skeleton, so calling it a second time within a tick could
double-apply something. At alpha=1.0 the substituted pose IS the game's pose, so the sub-frame must
still reproduce the following main frame:

    identity  alpha=1, sub vs following main : 3,257 px / mean 0.066   (was 3,116 / 0.075)
    reach     kick=3000 vs unkicked          : 7,334 px / mean 0.303   (was 2,477 / 0.001)
    actor alpha 1.0 vs 0.0, camera pinned    : 5,608 px / mean 0.070   (was 0 — byte-identical)

Identity is unchanged (its mean actually falls slightly), so the extra call has no side effect worth
seeing. And the kicked pixels are POSITIVELY IDENTIFIED as the player rather than assumed: a single
compact blob, bounding box 113 x 238 at x 591..703 / y 479..716 — screen-centre, lower half, aspect
2.11, filling 27% of its box. That is a standing character silhouette where Mario stands, not a
scatter of props.

The actor alpha now moves 5,608 pixels where it moved zero. That is Mario's own 2.42 units of
per-tick motion reaching the frame, at the right scale for a character occupying ~7,300 px.

### Still opt-in, and why

One present, one scene, one moment. The identity and reach numbers are clean but they are not a
sweep, and `calcAnim` is a large function with callback side effects that a single sample cannot
exonerate across cutscenes, Yoshi, water and the Torocco path (`calcBaseMtx` has four distinct
branches, only one of which was exercised here). Default-on wants the gate run across several
moments first. The remaining 400-object scenery set is still gated behind the calc-anim boundary
and its own cost — that is the next piece, and it is separate from the player.
