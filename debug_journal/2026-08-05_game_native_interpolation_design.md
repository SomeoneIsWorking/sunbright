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
