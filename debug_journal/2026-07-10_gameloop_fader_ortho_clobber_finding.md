# 2026-07-10 (continuation) — title black backdrop: order-divergence hypothesis FALSIFIED;
# real proximate cause is `TApplication::gameLoop`'s unconditional fader ortho rebind

Task framing at session start (based on a new retail FIFO capture,
`scratch/oracle/fifo/title_press_start_vi_stable_gxseq.txt` + MANIFEST): retail rebinds a
fresh PERSPECTIVE projection ~1073 ops before the first draw of the captured frame; native's
phase-1 `unk40` ("Draw Buffer Group") draw-buffer flush was assumed (from
`2026-07-10_projection_carryover_root_cause.md` §3-4, captured with an `AFTER=1990`-gated
`SB_PLIST_ORDER_DBG`) to fire BEFORE `camera 1` ever rebinds perspective, i.e. a perform-list
*order* bug (camera dispatched after the drawbuf group instead of before).

## 1. Order hypothesis is FALSIFIED — `camera 1` (via `TMarDirector::preEntry`/`unk34`) already
binds fresh world-perspective immediately before `unk40`'s draws

Re-ran with **unconditional** `SB_PLIST_ORDER_DBG=1` (no `_AFTER` gate — the gate in the prior
session's capture only starts printing once `VIGetRetraceCount() >= threshold`, which
silently hid everything dispatched in the retrace *before* the gate opened, including the
entry pass that precedes the render call it captured). Full, ungated per-call sequence
(`scratch/logs/order_proj_probe.log`, steady-state stage-15 title, retrace 19882→19884):

```
[plist-order] n=6452979 retrace=19882 name="vp WParticle 2"      flags=0x8
[plist-order] n=6452980 retrace=19882 name="camera 1"             flags=0x10   <- preEntry() line 30
[proj-dbg]    n=129251  fn=GXSetProjection type=P retrace=19882 diag=[2.041635,2.747478,...]  (WORLD camera, correct)
...
[plist-order] n=6452986 retrace=19882 name="camera 1"             flags=0x10   <- preEntry() line 54
[proj-dbg]    n=129252  fn=GXSetProjection type=P retrace=19882 diag=[2.041635,2.747478,...]  (same, correct)
...
[plist-order] n=6452993 retrace=19882 name="マリオ"                flags=0x8000000  <- preEntry() line 74, list ends
[proj-dbg]    n=129253  fn=GXSetProjection type=O retrace=19882 diag=[0.004464,-0.003125,...]  <- NOT logged as any
                                                                                                    testPerform dispatch
[plist-order] n=6452994 retrace=19884 name="Draw Buffer Group"    flags=0x8   <- unk40, phase 1, FIRST draw of new frame
[plist-order] n=6452995 retrace=19884 name="DrawBuf Sky Opa"      flags=0x8
...
```

`TMarDirector::preEntry` (`MarDirectorPreEntry.cpp`, populates `unk34`, dispatched as the
"ENTRY pass" at the tail of every `TMarDirector::direct()` call per `MarDirectorDirect.cpp`
line 270) pushes `camera1` with flag `0x10` **three times** (lines 30, 54, 70) — the FIRST
push (line 30, immediately before the Sky/Map/Mirror/Chr `0x480` collect pushes) is exactly
the re-primer the black-backdrop investigation was looking for, and it fires correctly, in
the correct position (before `unk40`'s subsequent draws, not after) — matching retail's own
`camera1`/DrawBuf ordering in `preEntry`'s source. **The (a)/(b)/(c) order-divergence
hypotheses in this session's task brief are wrong: perform-list construction order matches
retail's decomp exactly; nothing is reversed, mis-attached, or early-out-ing at the
NameRef/perform-list level.**

## 2. The real last writer before `unk40`'s draws: `TApplication::gameLoop`'s own fader ortho

The `type=O` bind at `proj-dbg n=129253` — landing between `preEntry`'s last entry and
`unk40`'s first draw, with **no corresponding `[plist-order]` line at all** — is not part of
the perform-list/`TViewObj::testPerform` dispatch tree. Its actual caller, found with the
existing (previously under-used) `SB_PROJ_BT=1` instrument (uncapped per-call backtrace, as
opposed to `SB_PROJ_DBG_AFTER`'s 5-per-type cap which had been silently truncating every
previous session's attempt to see this call — the cap was exhausted before reaching this
specific occurrence each time):

```
[proj-call] n=249 type=O
GXSetProjection+0x6b
TApplication::gameLoop()+0x1e5     <-- direct caller, NOT via TPerformList::perform
TApplication::proc()+0xc4
aurora_main+0x1ad
```

This is `reference/sms/src/System/Application.cpp:851-866` (decomp source, not
native-only — no `#ifdef SMS_NATIVE_PLATFORM` anywhere in this block):

```cpp
nextState = mDirector->direct();          // <- may be a render call (phase1..6) or entry-only

JDrama::TVideo* video = mDisplay->unk60;
GXSetViewport(0.0f, 0.0f, video->mNextRenderMode.fbWidth, video->mNextRenderMode.efbHeight, 0.0f, 1.0f);
GXSetScissor(0, 0, video->mNextRenderMode.fbWidth, video->mNextRenderMode.efbHeight);
Mtx afStack_1ac;
C_MTXOrtho(afStack_1ac, 0.0f, (f32)video->mNextRenderMode.fbWidth, 0.0f, (f32)video->mNextRenderMode.efbHeight, -1.0f, 1.0f);
GXSetProjection(afStack_1ac, GX_ORTHOGRAPHIC);   // unconditional in this decomp
mFader->update();
mFader->draw(...);
```

This runs **every single `gameLoop()` iteration**, unconditionally, right after
`mDirector->direct()` returns — including the iteration where `direct()` just finished its
render pass AND its trailing entry pass (`unk34`/`preEntry`, confirmed §1 to correctly rebind
world-perspective at its own tail). Verified via `TMarDirector::direct`'s control flow
(`MarDirectorDirect.cpp`): in steady state, one call to `direct()` does BOTH the full render
(phase1 `unk40` → ... → phase6 `mPerformListGXPost`) AND the tick+`preEntry`/`unk34` pass for
the *next* frame, all within the same call (the `for(;;)` loop's `0x4000` flag carries the
render/entry split across iterations of the SAME call, not across separate calls — confirmed
by disassembly of `TMarDirector::direct()`: `sb_boot_capture_set_phase(1..6)` calls are
sequential in one linear code path, and `preEntry`'s three unique `unk34`-only literal
strings — `水マネージャ`/`水飛沫マネージャ`/`クエッションマネージャ`/`プレーヤーグループ` — appear
immediately after phase 6 completes, in the SAME captured retrace, before the retrace counter
increments). So the true per-frame order is:

```
direct(): [render phase1..6, using proj left over from THIS iteration's PRIOR gameLoop call]
          [tick(s) + preEntry/unk34: rebinds world-perspective + collects next frame's buffers]
          return
gameLoop: GXSetProjection(ORTHOGRAPHIC)  <- unconditional fader compositing setup, clobbers
          mFader->draw(...)                the perspective preEntry JUST bound
          present
[NEXT direct() call]: render phase1 (unk40) draws FIRST, consuming gameLoop's leftover ORTHO
```

**This is the actual, most-proximate, byte-verified mechanism**: every `unk40`
("Draw Buffer Group" — Sky/Map/StaticMapObj/Graffito/Mirror/Chr/LensFlare/Indirect/Light
draw buffers, i.e. the entire 3D world) draw-buffer flush consumes whichever projection
`TApplication::gameLoop`'s fader-compositing block left bound, not the perspective `preEntry`
correctly rebinds moments earlier — because gameLoop's fader block runs strictly *after*
`direct()` returns, unconditionally, on every iteration.

## 3. Open question — is this ALSO retail's real structure, or a decompile gap?

This block (`Application.cpp:851-866`) is unmodified decomp source with no visible
conditional gating `GXSetProjection(...,ORTHOGRAPHIC)` on the fader's actual visibility/state
(no `if (mFader->isActive())` or similar wraps it). If real hardware executes this literally,
retail would show the identical clobber — contradicting the oracle FIFO capture's own
headline finding (fresh PERSPECTIVE ~1073 ops before the first draw, not a leftover ortho).
Dispatched a Ghidra decompile of the real, compiled `TApplication::gameLoop` (US retail DOL,
`FUN_802a5f50 @ 0x802a5f50`, identified via the caller return address `0x802a6170` already on
record in `debug_journal/2026-06-11_choppy_music.md`). **Result: genuinely unconditional,
byte-for-byte matching the decomp's control-flow shape — no dropped `if`.** All three
sub-cases (`APP_STATE_BOOT` / `APP_STATE_NLOGO` / the `mDirector->direct()` dispatch) merge
unconditionally into the same `GXSetViewport→GXSetScissor→C_MTXOrtho→GXSetProjection(...,1)`
block in the shipped retail binary, identical in shape to the doldecomp source. Decompiled
source saved at `build/decomp/802a6170.c` (function `FUN_802a5f50`). **§2's mechanism is
NOT a native-only decompile gap — retail hardware genuinely executes this same unconditional
fader-ortho rebind every `gameLoop` iteration.**

## 4. Conclusion — real remaining defect is a pacing/cadence divergence, not a code-structure bug

Since (a) `preEntry`/`unk34` correctly rebinds world-perspective at its own tail (§1), (b) that
rebind happens strictly BEFORE `gameLoop`'s per-iteration fader-ortho clobber (both are
unconditional, decomp-faithful, and now Ghidra-confirmed identical to retail — §2-3), the
*only* remaining variable that can explain retail showing a fresh perspective near its
frame's head (before its own draws) while native's phase-1 draws inherit the fader's stale
ortho is **which of native's `TMarDirector::direct()` calls (i.e. which `gameLoop` iteration)
actually contains the render (phase1-6) vs. the tick+`preEntry`/`unk34` entry pass, relative
to real VI frame boundaries** — i.e. a **pacing/cadence divergence in the `unk54 -=
vsyncRate` tick-accumulator**, not a structural/ordering bug in the perform-list or
`Application.cpp` code itself. If native's accumulator causes the render call and the
immediately-preceding `preEntry` entry pass to land in DIFFERENT relative positions around
`gameLoop`'s fader-ortho block than retail's real per-VI-frame cadence does, phase1 would
consume the wrong side of that clobber even though every individual piece of C++ is faithful.

**This is the "larger" case, named per instruction — not fixed this session.** Confirming it
needs comparing native's actual `gameLoop`-iterations-per-VI-retrace cadence against retail's
(a Dolphin oracle instrumentation of call frequency vs. `VIGetRetraceCount`, matching this
project's existing oracle-tooling class) — a further RE task, not a guessable one-line patch.
Per the no-bandaids rule, no fix was applied to `Application.cpp` or `MarDirectorDirect.cpp`
this session: every concrete hypothesis this session could mechanically test (list-insertion
order, wrong-phase attachment, a dropped retail conditional) was tested and falsified: the
code is faithful to retail at the structural level checked. Inserting a manual re-prime call
or reordering these dispatches now, without pinning the cadence divergence first, would be
exactly the "insert a call to make output line up" pattern this project bans.

## New diagnostics used (already existed, previously under-exercised)

- `SB_PLIST_ORDER_DBG=1` used **unconditionally** (no `_AFTER` gate) — the gated form silently
  hides everything before the gate opens, which is exactly the entry-pass evidence this
  session needed. Prefer the ungated form (`grep`-filtered afterwards) over `_AFTER` when the
  question is "what happened in the retrace(s) immediately before X", not just "what happens
  at X".
- `SB_PROJ_BT=1` (uncapped per-call backtrace) used instead of `SB_PROJ_DBG_AFTER` (5-per-type
  cap) — the cap had been silently exhausted before reaching the specific occurrence every
  previous session needed a backtrace for. `SB_PROJ_BT=1` is the correct instrument whenever
  a *specific* call (not "the first few of a type") needs attribution.
