# 60fps — the map, and the unification plan

The project grew **three independent interpolated-60fps implementations**, none of which knows the
others exist. They share no code, no switch, no vocabulary, and each one covers a different part of
the frame — which is why turning "60fps" on can look *worse* than 30: whichever path you enable is
missing whatever the other two solved.

This file is the map. It exists because "make sure all 60fps hooks are clearly indicated" is not a
thing a reader can currently get from the source: the hooks are spread over eight files under three
different names (`lerp60`, `interp60`, `interp60_replace`), and 49 environment switches select
between them.

**Target: ONE path, shaped like dusklight's `src/dusk/frame_interpolation.{h,cpp}`.** See
"Unification" at the bottom for what survives and what goes.

---

## STATUS — what interpolates today (2026-08-07)

`SBR_LUCENT_DEBUG=interp` prints this live. Every draw is filed under the system that emitted it
(`GX_AURORA_DRAW_POP`) and the fate it received, at the point that fate is decided; the outcomes are
exhaustive, so the columns sum to the draw count.

| population | interpolates | note |
|---|---|---|
| shadow model (ship / pass-4) | **99.8%** | per-model ordinal |
| J3D shape (world) | **97.3%** | `(shape, instance)` tag at `J3DShape::draw` |
| shine shadow slice | **95.4%** | per-slice ordinal |
| JPA particle | **95.3%** | own motion as a translation |
| shadow volume | **94.8%** | keyed by the OWNING ACTOR |
| flag (deforming) | **100%** | VERTEX interpolation |
| sea ripple (deforming) | **100%** | VERTEX interpolation |
| shadow alpha cube | 0% | not investigated |
| text glyphs, J2D pane | — | **CORRECT**: 2D has no meaningful in-between |
| (unlabelled) | — | the audit's own edge; kept visible on purpose |

### Deforming geometry — the vertex path (2026-08-07)

Flags and the sea ripple grid rebuild their mesh every tick, so their motion lives in the VERTEX
DATA and no matrix reaches them. `interp::patch_vertices` lerps the positions directly: 86,910 of
107,813 offered draws (80.6%), with both populations at 100% of what the audit can see.

Three things this had to get right, each of which fails silently otherwise:

* **A separate buffer.** Both emissions replay the same recorded passes and therefore the same
  `vertRange`, so patching in place corrupts the tick's OWN frame. Uniforms escape this because the
  snapshot re-pushes them; vertices have no such path. The lerp is pushed as new vertex data and
  only the interpolated emission's command is repointed at it.
* **Big-endian floats.** The buffer holds raw GC vertex records — the shader byte-swaps them
  (`gx/shader.cpp` `bswap32`). A lerp on the native interpretation of those bytes produces garbage
  that still renders.
* **It must run LAST.** "Direct f32 positions and a tag" also describes a JPA billboard, whose
  positions are baked in EYE space; lerping those across ticks mixes two view transforms. Running
  the vertex path first did exactly that and moved 516,562 particle draws off the correct
  billboard-translation path onto this one. It is now the fallback after the specific paths, and
  particles are back at 414,241 billboard / 0 vertex.

A draw whose vertex COUNT changed between ticks is snapped, not smeared between two unrelated
meshes (7,882 per run), and one with no consecutive previous tick snaps too (20,903).

`camera-only` is an **upper bound** on the defect, not a measurement of it: for STATIC world
geometry the camera delta is exactly correct, and no sound test to separate the two exists yet. Two
were tried and both were unsound — see `interp.cpp`. A previous version of this table read
particles at 99.4% and the shine slices at 100% on the strength of the first one; those numbers were
an artefact and are withdrawn.

**Mispairing is 16 against a no-tagging control of 4** — 0.002% of tagged draws, so none of that
coverage is bought with wrong pairings, which is the trade this arc made twice and had to undo.

### A verdict reversed by re-measuring after the base was fixed

The shine-slice and shadow-model rows sat at **0%** because their ordinal keys had been measured as
carrying "~93% of the added mispairing" and were withdrawn. That measurement was CONFOUNDED: it was
taken while the shadow tag read `r4` — a **bool** — so the shadow VOLUME was mispairing
catastrophically and the ordinals were blamed for its damage.

Re-measured with the owner key as the base:

| | mispairs | volume | shine | model |
|---|---|---|---|---|
| owner only | 16 | 94.6% | 0% | 0% |
| owner + ordinals | 16 | 94.6% | **95.4%** | **99.8%** |

Identical mispairing, ~730,000 more draws interpolating. An ordinal is still a positional stand-in
and still misaligns when a list changes length — it simply does not, measurably, in this game, and
that was invisible while the base key was broken.

**A measurement taken over a broken component is not evidence about the components beside it.** Two
verdicts in this arc stood on exactly that.

---

## The three paths

| | A — stream interpolation | B — substitute & re-issue | C — record & replace |
|---|---|---|---|
| switch | `SBR_60FPS` (alias `SBR_LERP60`) | `SBR_INTERP60` + `_ALPHA` | `SBR_INTERP60_REPLACE` |
| where | `runtime/lerp60.{h,cpp}` → `aurora::gfx::interp` (`extern/aurora/lib/gfx/common.cpp`) | `overrides/interp60_snapshot.cpp` (2951 lines) | `overrides/interp60_replace.{h,cpp}` |
| mechanism | record the tick's GX stream, rewrite the **recorded frame's matrices** in uniform staging toward the previous tick, present the packet twice | write an interpolated pose into the game's own objects, **re-run** the tick's draw lists from it, restore | record each `J3DModel`'s final draw matrices, **overwrite** the live buffer with `lerp(prev,cur)` for the duration of the sub-frame's draw, restore byte-exactly |
| runs game code in the sub-frame | **no** | yes — the whole draw-list set | yes — the whole draw-list set |
| identity/pairing | per-draw TAG emitted at `J3DShape::draw` | actor object address | `(J3DModel, matrix index)` |
| **effects handled** | **yes** — EFB cross/intra-frame copies, camera cuts, screen-sampling effects, afterimage feedback | no | no |
| leak into guest state | none (never touches guest memory) | unbounded — chased for weeks | none by construction |
| honest coverage number | yes: tagged/untagged draws split ortho vs persp vs indexed | no | yes: the `NOT covered` line |
| status | **most complete**; the one whose effects work | superseded | best-measured, worst-covered |
| **JUDDER** (measured, matched ticks) | **1.10** | — | **2.33** |

### ⚠ PRESENT MODE IS PART OF THE MECHANISM

An interpolated run MUST use a queued present mode. aurora's `vsync = false` selects **Mailbox**,
whose defining behaviour is that a newer present REPLACES the pending image — so a tick emitting two
images inside one display refresh has its in-between frame **discarded by the swapchain**, while
every counter still reads 60 fps. Interpolated runs therefore select `vsync = true` → strict
**`Fifo`**, where each presented image is queued and shown for at least one refresh.

Not `FifoRelaxed`, which was the first choice and is wrong here: its whole purpose is to present
IMMEDIATELY when a frame missed its vblank, and a late tick is the common case (94% measured), so
the pair went out back to back exactly as under Mailbox. `AURORA_PRESENT_RELAXED=1` restores it
for comparison.
`debug_journal/2026-08-06_interp60_mailbox_discards_the_inbetween.md`.

The corollary is that the frame-loop sleep which used to space the two presents is obsolete: the
display's refresh does the spacing. It is kept behind `SBR_MIDPOINT_SLACK` for comparison.

### Measured, on the axis that matches the complaint

`tools/interp/cadence.py` scores what a player reports. It labels no present as "main" or "sub" —
it takes the difference between each pair of CONSECUTIVE presents and asks whether those steps are
the same size. `judder = max(step)/min(step)`; 1.0 means every present advances the game equally.
Same scenario, same pad script, **matched guest ticks** (~4802–4818, camera rotating):

| configuration | judder | mean step |
|---|---|---|
| no interpolation, plain 30fps | 1.18 | 7.29 |
| **A** — stream interpolation | **1.10** | 5.13 |
| **C** — record-and-replace | **2.33** | 6.02 |

**C is twice as juddery as not interpolating at all**, and C is what `play.sh --60fps` used to
select. A is smoother than the 30fps baseline. That is the whole case for the unification target
below, and it is why the flicker is a structural mismatch rather than a tuning problem: C covers
`J3DModel` draw matrices and nothing else, so the 2D HUD, particles, immediate-mode geometry, the
dash-trail EFB feedback and every screen-sampling effect step at 30 Hz inside a 60 Hz frame, while
its sub-frame re-issues draw lists that were never meant to run twice.

RETRACTED, in the same session it was written: an earlier version of this table added a
"presents/tick" column reading "irregular, 2–3" for A and "1–3" for C, and called it judder by
construction. That came from grouping dumps by their `-t<n>` filename label — the GAME's own retrace
counter, which advances by however many NTSC fields the game asked for that frame, so two
consecutive ticks can share a label and a perfectly regular 2-per-tick cadence groups as 1, 2 or 3.
The runtime's own counters say **6000 in-between frames for 6000 simulation ticks: exactly two
presents per tick, regular**. The measurement was right and the verdict was wrong, which is the
combination hardest to notice. `cadence.py` now labels that statistic for what it is and names the
runtime as the authority for cadence.

`play.sh --60fps` now selects A.

---

## Every hook, by address

Guest addresses are US. All of these are OBSERVE-ONLY wrappers (the real body always runs) unless
marked otherwise.

### Path A — stream interpolation (the effects-aware one)

| Address | Symbol | File | What it is for |
|---|---|---|---|
| `0x802e0390` | `J3DShape::draw` | `overrides/j3d_capture.cpp` | emits the per-draw IDENTITY TAG that lets aurora pair a draw across ticks; untagged draws snap |
| `0x800335d4` | `CPolarSubCamera::warpPosAndAt(Vec&,Vec&)` | `overrides/camera_cut.cpp` | the game's own camera-discontinuity signal → `snap_next_interpolation()` |
| `0x80033390` | `CPolarSubCamera::warpPosAndAt(f32,s16)` | `overrides/camera_cut.cpp` | same signal, ratio/yaw overload |
| `0x8019f83c` | `TShimmer::perform` | `overrides/screen_effects.cpp` | heat haze — records that it SAMPLED the screen this frame |
| `0x8027c12c` | `TModelWaterManager::drawRefracAndSpec` | `overrides/screen_effects.cpp` | water refraction — same |
| `TAfterEffect::perform` | dash blur | `overrides/widescreen_effects.cpp` → `overrides/afterimage.cpp` | identifies the EFB copy that is CROSS-FRAME feedback, so it advances once per tick and not twice |
| `TBathWaterManager::draw_mist` | mist | `overrides/widescreen_effects.cpp` | screen-sampling notification |
| `TMirrorCamera::perform` | mirror pre-render | `overrides/widescreen_effects.cpp` | screen-sampling notification |

Non-hook seams: `overrides/native_frame.cpp` calls `sbr_afterimage_tick()`,
`sbr_gxfifo_view_matrix()`, `sbr_camera_cut_take()` → `snap_next_interpolation()`, and
`sbr_camera_mode_tick()` once per tick; `host/main.cpp` arms `sbr_lerp_enabled()` before the first
frame is recorded.

### Path B — substitute & re-issue

| Address | Symbol | File | What it is for |
|---|---|---|---|
| `0x802f80d0` | `JDrama::TDisplay::endRendering` | `overrides/interp60_snapshot.cpp` | the sub-frame's EFB→XFB copy |
| `0x802a4e28` | `TPerformList::perform` | `overrides/interp60_snapshot.cpp` | records the tick's ordered draw lists, which the sub-frame re-issues |
| `0x802deeb8` | `J3DModel::viewCalc` | `overrides/interp60_snapshot.cpp` | counts model-view rebuilds; **also the recorder for path C** |
| `0x802fcc94` | `JDrama::TViewObj::testPerform` | `overrides/interp60_snapshot.cpp` | snapshots each actor's transform before its movement |

Also inside B, not as `SB_OVERRIDE`s: `camera_apply`/`camera_restore` (pose lerp + `C_MTXLookAt`
rebuild), `TMario::calcAnim` (US `0x80244800`) re-call for the player, and the `PreEntry` view-calc
re-issue.

### Path C — record & replace

No hooks of its own. It is driven entirely from inside B's seam (`sbr_interp60_subframe`) and
records from B's `J3DModel::viewCalc` hook. **C cannot run without B**, which is itself a reason the
two need merging rather than choosing between.

---

## Switches, by path

49 in total. The ones that select behaviour, as opposed to printing something:

| Path | Selects behaviour |
|---|---|
| A | `SBR_60FPS`, `SBR_LERP60`, `AURORA_REPLAY_PRESENT`, `AURORA_INTERP_ALPHA`, `SBR_FORCE_DASHBLUR` |
| B | `SBR_INTERP60`, `_ALPHA`, `_ALPHA_CAM`, `_ALPHA_ACT`, `_COPY`, `SBR_PRESENT_AFTER_COPY`, `_ACTORS`, `_PLAYER`, `_ANIM`, `_CALCANIM`, `_PREENTRY`, `_PREENTRY_VC`, `_PREENTRY_VC_N`, `_PREENTRY_VC_CUE`, `_MASK`, `_DROPLAST`, `_NORESTORE`, `_KICK`, `_FOLLOW`, `_J3DSYS` |
| C | `SBR_INTERP60_REPLACE`, `_REPLACE_ALPHA`, `_REPLACE_NONRM`, `_REPLACE_KICK`, `_REPLACE_KICK_ONLY` |

The remaining ~24 (`_CENSUS`, `_VCLIST`, `_LISTS`, `_VIEWCALC`, `_TRACE`, `_MTXTRACE`, `_CAMTRACE`,
`_ORDER`, `_STREAMHASH`, `_ACTCENSUS`, `_MTXCENSUS`, `_PLAYER_DIFF`, `_VIEWSEQ*`, …) are
diagnostics from the investigation. They belong behind `SB_LOG` channels, not behind their own env
vars — the project already has one tracked logger and this arc bypassed it 24 times.

---

## Harness

| Tool | What it does |
|---|---|
| `tools/interp/interp60_run.sh` | the one runner; carries the whole switch set, prints the MOTION CENSUS at the dumped moment before any score |
| `tools/interp/subframe_position.py` | scores a sub-frame's position between its two neighbours (asymmetry / lead / off-segment); `--selftest` forces five cases |
| `tools/interp/frame_regions.py` | WHERE two frames differ — tile grid, coverage, top-decile concentration |
| `tools/interp/cadence.py` | **JUDDER** — how evenly consecutive presents advance the game, plus presents-per-tick from the dump labels. Path-agnostic (labels nothing "sub"), so it is the one axis all three paths can be compared on. Refuses a comparison whose series do not overlap in guest time |
| `tools/interp/interp60_gate.sh` | the regression gate |
| `SBR_INTERP60_CENSUS=1` | per-tick displacement of the drawn matrices — the one liveness probe that watches no named object |

---

## Unification — the target shape

Follow dusklight (`~/repo/dusklight/src/dusk/`, CC0), which solved this once already:

**LANDED** — the directory exists and the build is behavior-identical across the move (judder 1.10,
mean step 5.131, same guest ticks, before and after). What is in it today:

```
sms-recomp/frame_interp/          <- ALL 60fps code, one CMake OBJECT library
  frame_interp.{h,cpp}   THE public API. Mode (dusklight's Off/Capped/Unlimited), the step,
                         request_presentation_sync(), add_interpolation_callback(), and the
                         per-run report with its denominators.        [NEW, live]
  stream_interp.{h,cpp}  path A's driver + tag-coverage report        (was runtime/lerp60.*)
  camera.cpp             camera-cut detection -> presentation sync    (was overrides/camera_cut.cpp)
  effects.h              the screen-effect registry interface         (was runtime/screen_effects.h)
  effects_screen.cpp     shimmer / water refraction identity          (was overrides/screen_effects.cpp)
  effects_afterimage.cpp dash-trail EFB cross-frame feedback          (was overrides/afterimage.cpp)
  record_replace.{h,cpp} path C: matrix record + lerp + census        (was overrides/interp60_replace.*)
  subframe_legacy.cpp    path B, 2951 lines, superseded               (was overrides/interp60_snapshot.cpp)
```

Wired seams, all three real and all three verified firing:

| seam | called from | evidence |
|---|---|---|
| `begin_sim_tick()` | `present_tail` in `native_frame.cpp` | 6000 ticks counted over a plaza run |
| `present_interpolated_frame()` | `aurora_replay_midpoint()` — the one point genuinely BETWEEN a tick's two presents | 6000 in-between frames, 1:1 with ticks |
| `request_presentation_sync()` | `camera.cpp` via `present_tail` | 0 requests, and the report SAYS 0 rather than staying silent |

The callback registry is live and **nothing registers yet** — the report says so in those words,
because "no effect asked to be interpolated" and "the dispatch never ran" are the same silence
otherwise. Porting the effects onto it is the next step.

The target this is converging on:

```
sms-recomp/frame_interp/
  frame_interp.h      ONE public API, dusklight's shape:
                        begin_sim_tick() / begin_frame(mode, is_sim_frame, step)
                        begin_record() / end_record() / interpolate()
                        request_presentation_sync()      <- a frame that must be EXACT
                        add_interpolation_callback()     <- how a system opts in
                        is_enabled() / is_sim_frame() / get_interpolation_step()
  frame_interp.cpp    mode, step, tick sequencing, sync, callback dispatch
  record.cpp          the recording + replacement table
  camera.cpp          camera interpolated as a POSE (eye/center/up/bank/fovy/near/far), never as a
                      matrix lerp — dusklight's split, for the reason dusklight gives
  effects.cpp         EFB cross/intra-frame copies, screen-sampling effects, cuts  (path A's work)
  diagnostics.cpp     census, coverage, kick controls — every probe, one place
```

with **one** user-facing setting, dusklight's enum:

    FrameInterpMode { Off, Capped, Unlimited }

The env name is deliberately NOT written here yet. This repo's commit gate fails a build
when an instruction names a switch no code reads, and it caught exactly that on the first
draft of this file — a documented switch that does not exist sends the next session
chasing it. The name goes in when the code that reads it does.

Decisions this map settles:

1. **Path A's MECHANISM survives.** Rewriting the recorded frame's matrices runs no game code in the
   sub-frame, which is why its effects work and why it cannot leak. Paths B and C both re-issue draw
   lists — the source of the flicker — and B additionally writes guest state as an input.
2. **Path C's PAIRING survives.** `(model, index)` is the correct key (`viewCalc` begins with
   `swapDrawMtx()`, so a matrix address alternates between ticks), and its recording is the honest
   one: it knows what it does not cover and prints that list every time it prints a number.
3. **Path B is deleted** except for the `TPerformList::perform` and `testPerform` hooks if the merged
   path still needs them, and except for its measurement harness, which is good and stays.
4. **Effects opt in via `add_interpolation_callback`** rather than being enumerated by the
   interpolator. That is dusklight's inversion and it is what stops the effect list going stale.
5. **`request_presentation_sync()` replaces `snap_next_interpolation()`** — same idea, dusklight's
   name and semantics (step forced to 1.0, every replacement lookup disabled for that frame).
