# 2026-07-10 (continuation) — title black backdrop: root cause is a cross-frame projection carry-over, not a per-draw ortho bug; two blocking crashes found+fixed along the way

Task: instrument `GXSetProjection` (`SB_PROJ_DBG`/`SB_PROJ_DBG_AFTER`), find who binds the
wrong projection for the Sky/Map/Mirror 3D draws at the stable stage-15 title window, and
either fix a small unambiguous defect or name a larger one precisely.

## 0. Two blocking crashes hit before any projection evidence could be collected (fixed)

Neither is the audio arc's known "silent by omission" gap — both are genuine null-deref/UB
bugs that crash ANY run reaching the relevant code, turbo or not, before present ~2000.

### 0a. `JAIBasic::releaseControllerHandle` null-deref on an unlinked handle (FIXED)

`SB_HEADLESS=1 SB_STAGE=15 SB_TURBO=1` crashed deterministically (SIGSEGV, `eu-stack --core`
— **`gdb` 17.2 on this Fedora 44 box double-frees its own tcache reading this binary's core;
use `eu-stack` instead, it works cleanly**) inside:

```
JAIBasic::releaseControllerHandle
JAIBasic::stopSeq
MSBgm::stopTrackBGMs
MSound::exitStage
TMarDirector::~TMarDirector
TApplication::proc
```

`stopSeq`'s existing 2026-07-09 native guard (`JAIGFrameSequence.cpp:26-39`, itself a marked
stopgap for the unported JAS audio arc) detects `getSeqParameter()==nullptr` and does
"handle-side cleanup" — but that cleanup calls `releaseControllerHandle(&unk0->unk210,
param_1)` unconditionally. `releaseControllerHandle` (`JAIBasic.cpp:820`) assumes the handle
is spliced into the buffer's doubly-linked in-use list (real GC invariant: every handle here
came through `getControllerHandle`, so `sound->unk2C` is only null when the handle IS the
list head). Under the unported-audio path, a BGM handle can reach `stopSeq` never having been
through that splice — `unk2C` is null while `buffer->unk4 != sound`, so `sound->unk2C->unk30`
null-derefs.

**Fix** (`reference/sms/src/JSystem/JAudio/JAInterface/JAIBasic.cpp`): guard the
`SMS_NATIVE_PLATFORM` branch with `if (sound->unk2C != nullptr)` before the deref; the
`else` branch (list head) is untouched, and non-native keeps the original unconditional
deref. Marked as a stopgap paired with the existing `stopSeq` one — both delete together
when the JAS mixer arc lands and every handle here is genuinely list-linked.

### 0b. Not a bounce loop — this is a first-setup crash

`debug_journal/2026-07-09_title_bounce_loop_and_heap_find_offbyone.md` already fixed a
MOVIE/GAMEPLAY bounce loop and reported "attract loop cycles cleanly." Confirmed this
session's crash is **not** that loop recurring: `MapObjFlagManager` appears exactly 8 times
per run (8 objects in ONE scene load, matching that journal's own correction), not 8 bounce
iterations. This crash fires on the ordinary first stage-15 director teardown/reconstruct
that's part of ordinary `TApplication` state-machine setup — any teardown of any
`TMarDirector` whose BGM handle never went through `getControllerHandle` hits it.

## 1. `SB_PROJ_DBG` / `SB_PROJ_DBG_AFTER` landed (aurora, `GXTransform.cpp`)

Per-call log of every `GXSetProjection`/`GXSetProjectionv`: type, the 4 diagnostic terms
(`mtx00, mtx11, mtx22, mtx23`), and `VIGetRetraceCount()` (weak extern, same counter
`SB_DUMP_FRAME_AFTER`/`SB_NDC_PROBE_AFTER` use). `SB_PROJ_DBG_AFTER=<retrace>` additionally
dumps a caller backtrace for the first 5 calls of EACH projection type once that retrace
clears — `SB_PROJ_DBG=1` alone logs every call unconditionally (verbose, used to get a
complete non-capped sequence via `grep`).

## 2. `SB_PLIST_ORDER_DBG` / `SB_PLIST_ORDER_DBG_AFTER` landed (reference/sms,
`JDRViewObj.cpp`)

`TViewObj::testPerform` is the one chokepoint every perform-list dispatch passes through
before calling the target object's `perform()` virtual (`JDRViewObj.cpp:8-36`). Added a log
of `(sequence#, retrace, NameRef name, filter bits)` right before the dispatch — this is
what let a `GXSetProjection` backtrace (which only resolves generically to
`TPerformList::forEachPerform → testPerform → perform` with the concrete callee's identity
erased by the vtable call) be matched to an exact named scene object in exact per-frame
order.

## 3. What the two instruments together show, at the stable present (retrace=1990,
turbo, `SB_HEADLESS=1 SB_STAGE=15`)

Full per-frame dispatch order (`scratch/logs/run8_filtered.log`, `SB_PROJ_DBG=1
SB_PLIST_ORDER_DBG_AFTER=1990`), abbreviated to the load-bearing lines:

```
n=1   "Draw Buffer Group"     flags=0x8     <- unk40 (phase 1, TMarDirector::direct's FIRST perform-list every frame)
n=2   "DrawBuf Sky Opa"       flags=0x8     -> J3DDrawBuffer::draw() fires HERE (see below)
n=3   "DrawBuf Sky Xlu"       flags=0x8
n=4   "DrawBuf MapOpa"        flags=0x8
...   (all ~28 DrawBuf objects draw() here, under WHATEVER projection was last bound)
n=36  "camera 1"              flags=0x10    -> [proj-dbg] type=P diag=[1.52,2.05,...] (mirror camera's own P)
n=72  "camera 1"              flags=0x10    -> [proj-dbg] type=P diag=[2.0416,2.7475,-0.000033,-10.0003]  <- WORLD camera
n=80  "DrawBuf Sky Opa"       flags=0x8     (SAME object as n=2 -- this is its OWN later re-entry, unrelated)
...
n=249 "<TOrthoProj>"          flags=0x10    -> [proj-dbg] type=O diag=[0.004464,-0.003125,-0.5,-0.5]  <- 2D UI screen
n=252,273,279,286  more <TOrthoProj>        -> same O, end-of-frame 2D panels
n=601 "camera 1"              flags=0x10    -> [proj-dbg] type=P diag=[2.0416,2.7475,...]   <- world camera, NEXT frame's mPerformListGX pass
n=603 "DrawBuf Sky Opa"       flags=0x480   (0x400|0x80 = frameInit+setDrawBuffer -- COLLECTS next frame's shapes, does NOT draw)
n=604 "DrawBuf Sky Xlu"       flags=0x480
n=608 "DrawBuf MapOpa"        flags=0x480
```

`TDrawBufObj::perform` (`JDRDrawBufObj.cpp:68-158`) is unambiguous about what each bit does:
`0x80`→`frameInit()`, `0x400`→`setDrawBuffer()` (collect shapes for the NEXT draw), `0x8`→
`mDrawBuffer->draw()` (**the actual raw-GX emission**, `J3DDrawBuffer.cpp:301`). So:

- **`flags=0x8` at n=2-35 is the REAL GX emission** for Sky/Map/Mirror/etc — it fires at the
  very TOP of the frame's perform-list (`unk40`, phase 1), **before `camera 1` (n=36/72) has
  set anything for this frame**. `J3DDrawBuffer::draw()`/`drawHead()` never call
  `GXSetProjection` themselves (grepped the whole file — no such call); they rely entirely on
  ambient GX state.
- **`flags=0x480` at n=603+ is NOT a draw** — it's `frameInit`+`setDrawBuffer`, i.e. collecting
  THIS frame's shapes into the buffer that `unk40` will `draw()` at the TOP of the **next**
  frame (the standard J3D one-frame-latency double-buffered draw buffer). My earlier reading
  in this same investigation (that 0x480 was the "real emit") was backwards — corrected here.
- Confirmed directly against `SB_DRAW_DUMP_AFTER=1990` (drain-time capture, independent of
  emission-order tracing): `DrawBuf Sky Xlu`/`MapOpa`'s actual GX draw records show
  `proj=O prj=[0.0045 -0.0031 -0.5000 -0.5000]` — the exact ortho diagonal from `<TOrthoProj>`
  at the END of a frame, not `camera 1`'s perspective. This matches the emission trace: at
  n=2 (unk40's draw() call), the last thing anyone set GX's projection to was whatever ran at
  the tail of the PREVIOUS frame's `mPerformListGXPost` (phase 6) — the 2D UI's
  `<TOrthoProj>` entries — because nothing runs between one frame's phase-6 tail and the next
  frame's `unk40` head to reassert perspective.

## 4. Root cause, precisely

**The Sky/Map/Mirror draw buffers are drawn (`J3DDrawBuffer::draw()`, `unk40`/phase 1) using
whatever GX projection state was left bound by the END of the PREVIOUS frame's
`mPerformListGXPost` (phase 6) pass — and phase 6 legitimately ends with the 2D UI's
`TOrthoProj::perform()` (`JDRCamera.cpp:128-142`, unconditional `GXSetProjection(...,
GX_ORTHOGRAPHIC)`, no push/pop/restore anywhere in that function). Nothing in
`TMarDirector::direct()` (`MarDirectorDirect.cpp:290-317`, phases 1-6, all present and
unconditional in the decomp source, not native-only) re-primes the projection to perspective
between one frame's phase-6 tail and the next frame's phase-1 head.**

This is a genuine one-frame-latency architecture (J3D's real double-buffered draw-buffer
design — `TDrawBufObj::perform`'s three-bit split of frameInit/setDrawBuffer/draw is decomp
source, not a native invention) so the STRUCTURE itself is faithful. What's unverified is
whether GC hardware relies on the SAME carry-over (and something upstream I haven't found
yet re-primes perspective before phase 1 on real hardware) or whether a genuinely-dropped
perform-list entry is the missing re-primer.

**Candidate culprits among this session's `[plload] DROPPED` entries** (decoded Shift-JIS,
`PerformList.cpp:66-70` — a NameRef search miss permanently omits that entry from its list,
forever, for the whole run):

```
list='PerformList GX'      entry='PERFマップ描画'      (PERF map-drawing)
list='PerformList GX'      entry='PERFマップ系その他描画' (PERF map-series-other-drawing)
list='PerformList GX Post' entry='PERFキャラ描画'       (PERF character-drawing)
list='PerformList GX Post' entry='ブラーカメラ'         (Blur Camera)
list='PerformList GX Post' entry='PERF鏡ステージ'       (PERF mirror-stage)
list='PerformList GX Post' entry='PERFパーティクル描画'  (PERF particle-drawing)
```

None of these is literally named "camera" the way `camera 1`/`<TOrthoProj>` are (those two
dispatch fine, confirmed above) — but several are themselves NAMED SUB-PERFORMLISTS
("PERF" + category), and this project's own history flags exactly this class of bug already
(`MoveBG`/`TMapObjWave` "波" registration-order miss, referenced in `PerformList.cpp`'s own
comment). Whether one of these dropped sub-lists is what's supposed to re-run `camera 1`
(or an equivalent perspective-restoring call) between phase 6 and the next frame's phase 1
needs the retail PerformLists.bin membership (or a Dolphin GX-stream oracle capture of the
SAME title frame) to confirm — not a guess.

## 5. Stopped here per the no-bandaids rule

This is the **larger case**: I do not know, without an oracle capture of retail's actual
per-frame `GXSetProjection` sequence at this exact scene, whether (a) GC hardware has the
identical carry-over and something else I haven't located re-arms perspective before phase 1,
or (b) one of the dropped sub-performlists above is genuinely the missing re-armer. Patching
this by inserting a manual `GXSetProjection` call at the top of `unk40`'s dispatch, or by
force-loading one of the dropped names, would be exactly the "magic call to make output line
up" pattern this project bans — I have a concrete, evidence-backed mechanism but not yet the
retail ground truth for which side of it is wrong. Named precisely per instruction; next
session should pull a Dolphin oracle GX-stream capture (`tools/oracle/`, same class of tool
the 2026-07-07 arc built) of one title frame's full `GXSetProjection` sequence and diff it
against `scratch/logs/run8_filtered.log`'s sequence above.

## Frame verdict

`scratch/frames_title/proj_verify_1990.png` (SB_DUMP_FRAME_AFTER=1990, this session, HEAD +
the fixes above): SMS logo card renders correctly; area outside the logo/text is still pure
black. Symptom unchanged from `2026-07-10_title_backdrop_black_verdict.md` — this session
sharpens the mechanism (cross-frame projection carry-over via the phase-1/`unk40` draw-buffer
consuming stale phase-6 state) rather than the "every draw force-bound to one wrong ortho"
framing that verdict used; the exact per-draw evidence quoted there
(`proj=O prj=[0.0045 -0.0031 -0.5 -0.5]`) is the SAME value this session's mechanism explains,
just via cross-frame carry-over rather than an intra-frame binding bug.

## New diagnostics landed (permanent, env-gated)

- `SB_PROJ_DBG` / `SB_PROJ_DBG_AFTER=<retrace>` — `extern/aurora/lib/dolphin/gx/GXTransform.cpp`.
- `SB_PLIST_ORDER_DBG` / `SB_PLIST_ORDER_DBG_AFTER=<retrace>` — `reference/sms/src/JSystem/JDrama/JDRViewObj.cpp`.
- `eu-stack --core=<core> -e <exe>` noted as the working alternative to `gdb` for backtraces
  on this box (gdb 17.2 double-frees reading this binary's coredumps).
