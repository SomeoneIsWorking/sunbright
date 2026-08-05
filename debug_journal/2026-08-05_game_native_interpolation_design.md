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
