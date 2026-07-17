# 2026-06-24 — file-select selection → gameplay-request works; blocked on GAME_OVER-in-file-select

## Milestone (committed + pushed)
Drove the real file-block **selection** path to completion and ported the stage-transition
request. Three commits (submodule `fc70728`, `e6421dd`; parent `8f64739`, `78c3e01`):

  file-block screen (PROGRESS_UNK13) → head-butt block → setSelected → selectBookmark →
  selectFunction → firstStart (new-game) → **PROGRESS_UNK29 → setNextStage(0,nullptr)** →
  sets `unk4C |= 0x2` (pending stage change), `mNextArea = {stage 0, scenario 0xFF}`.

### Root-cause fixes (no bandaids)
1. **marioHeadAttack / marioHipAttack** (`MapObjLib.cpp`): empty decomp stubs (returned
   garbage); `TFileLoadBlock::touchPlayer` gates selection on `marioHeadAttack()`. Ported
   faithfully from GMSE01 `0x801b8df8` / `0x801b8d88` (gpMarioPos->y < block-bottom &&
   IsMarioStatusTypeJumping && *gpMarioSpeedY>0 ; GrPlane.getActor()==this && IsHipDrop && ...).
2. **gpMarioParticleManager->unk3B8** (`MarDirectorLoadResource.cpp`): the constructed
   `JPAEmitterManager` assignment was commented out (decomp gap) → null → head-butt sparkle
   emit deref'd null. Restored.
3. **gpEmitterManager4D2** (same file): constructed with a **null** resource manager, but its
   particle `0x1FA` (`ms_2d_pause_sel.jpa`, menu cursor glow) loads into `gpResourceManager`
   → createEmitter always failed → cursor-sparkle consumers deref'd an empty slot. Share
   `gpResourceManager`. Then **converted the prior session's `if(unkAC)` silent-skip bandaids
   to FAIL-FAST `OSPanic`** (a null emitter slot is now a real wiring failure, per user).
4. **selectFunction `unk2A4[3]` OOB** (`CardLoad.cpp`, 2 sites): decomp `i<4` over a `[3]`
   pane array reads `&unk2B0` (a JUTRect) as a `TExPane*` → would fault on real GC too.
   Bound to `i<3` (matches the case-0 loop). Same LP64-OOB class as titleDraw `i<13`.
5. **TMarDirector::setNextStage** (`MarDirectorEvent.cpp`): empty `// cursed` stub. Ported via
   Ghidra decompile (see "Tooling"): decodes the stage id into `gpApplication.mNextArea`,
   raises `unk4C` request bits (0x2 normal / 0x8 plaza-gate demo / 0x4 actor-driven),
   `mMovie=6` for stage 0x37.

## THE NEXT BLOCKER (precisely root-caused) — GAME_OVER in file-select
`setNextStage` correctly sets `unk4C |= 0x2`, but the transition never fires. Chain:
- The consumer (`MarDirectorDirect.cpp:971` `if (unk4C & 0x2) moveStage()`) lives in
  `updateGameMode()`, which `changeState()` calls **only when `mState == STATE_UNK4`** (line 424).
- The file-select director goes **STATE_UNK0 → STATE_UNK4 → STATE_UNK7** (`[dir-state]` trace,
  before the pick). The 4→7 hop is `updateGameMode` line 901: `if (unk4C & 0x20) r29 = STATE_UNK7`.
  `0x20` is set ONLY at line 877-878: `if (SMS_CheckMarioFlag(MARIO_FLAG_GAME_OVER)) unk4C |= 0x20`.
- So **Mario has GAME_OVER set during file-select.** Source: `TMario::perform` → `playerControl`
  → `thinkSituation` (`MarioMove.cpp:1966-1970`) runs the **death-plane check every frame**
  (it's not gated by MARIO_STATUS_WAIT — perform calls playerControl whenever `mFreezeTimer<=0`):
  `if (ground->isDeathPlane() && groundY > mPosition.y) onFlag(GAME_OVER)`.
- Mario is at world **(846, 0, -1000)** (verified via SB_SEL_POS). `gpMap->checkGround` there
  returns a **death plane** → GAME_OVER. In the real game Mario stands on the beach floor, so
  this never fires. **Our file-select scene collision is missing/mispositioned under Mario.**

### Why the collision is wrong (lead, not yet confirmed)
`SB_PLACE_DBG` proved **every** file-select placement (TPlacement::load) loads at **(0,0,0)** —
file blocks, the マリオ marker, etc. (only `camera 1` is nonzero, (0,1,0)). Yet Mario's runtime
pos is (846,0,-1000) (set elsewhere, not the placement). So the option-scene map collision is
likely loaded at the origin (or mis-parsed — cf. the `.col` BE/LP64 swap class) while Mario
stands at (846,0,-1000) off it → checkGround → death plane. NEXT SESSION: find where Mario
gets (846,0,-1000), and whether the option.arc map collision (`gpMap`) has a real floor there;
fix the scene collision / Mario spawn so `checkGround(846,0,-1000)` returns non-death ground.
Confirm by watching `[dir-state]` stay at 4 (not 7) and `[movestage] FIRED`.

## Repro (card already formatted → straight to UNK13 → auto-pick file 0)
    cd <repo-root>
    cmake --build build-native --target sms-boot -j$(nproc)
    pkill -9 -x sms-boot; S=""; for f in $(seq 200 12 1690); do S="$S ${f}:START $((f+6)):-"; done
    for f in $(seq 1760 80 6000); do S="$S ${f}:A $((f+20)):-"; done
    timeout -s KILL 200 setarch -R env SUNBRIGHT_DISC=scratch/disc/sms.iso SB_THP_FAST=1 SB_TURBO=1 \
      SB_HOST_ALLOC_CAP_MB=3072 SB_STAGE=15 SB_SCENARIO=0 SB_SEL_DBG=1 SB_SEL_PICK=0 SB_J3D_DBG=1 \
      SB_PAD_SCRIPT="$S" ./build-native/sms-boot > scratch/frames/fs.log 2>&1
    grep -a "prog unk1C\|dir-state\|movestage\|selpick" scratch/frames/fs.log

## UPDATE — GAME_OVER fully root-caused: Mario stands off the beach collision
Traced the GAME_OVER chain end-to-end (env `SB_DEATH_DBG`). The death-plane theory was
WRONG. Actual chain:
- `TMario::perform`→`playerControl`→`thinkSituation` runs the oob-kill EVERY frame: when
  `isTouchGround4cm() && mGroundPlane->isIllegalData()`, it accumulates `mOobKillTimer` and
  after `mIllegalPlaneTime` calls `decHP(mHpMax=8)` → HP 8→0 → `loserExec` → GAME_OVER.
  (Confirmed: `[hp]` health 8→0, `[oobkill] touch4cm=1 illegal=1 optionMap=1`.)
- `mGroundPlane->isIllegalData()` is true because Mario sits on the **illegal sentinel plane**
  (`mIllegalCheckData`, MapCollisionData.cpp:121) — `checkGround` finds **no triangle** under him.
- The option-scene map collision DOES load (`[mapcol]` numTri alloc=12000, **added=616**,
  gridExtent ±20480; `/scene/map/map.col` via TMapCollisionStatic, full BE/LP64 handling in
  TMapCollisionBase::init). The 616 triangles are VALID (e.g. p1(3622,60,-1575)… ground
  normals (0,1,0)) and ARE linked into grid cells (`[addgrid]` gga=1, cells computed).
- **THE MISMATCH:** the beach ground triangles are at **x≈3622-5484, z≈-1848..107** (grid
  cells x23-25). But Mario is clamped (`SMS_isOptionMap` path, MarioMove.cpp:2054-2059) to
  **x∈[846,2000], z=-1000** = `mOptionParams` (mZ,mXMin,mXMax). Those are the **PARAM_INIT
  placeholder defaults** (MarioInit.cpp:965-967: -1000/846/2000). The collision transform
  `unk20` is IDENTITY (raw vtx already world coords), so the collision really is at x≥3622 —
  Mario at x=846 is ~2776 off it → illegal ground → death.
- **ROOT CAUSE: `/Mario/Option.prm` is not overriding the default mZ/mXMin/mXMax**, so Mario's
  rail doesn't match the beach collision. NEXT: verify Option.prm loads (TParams("/Mario/
  Option.prm")) and that the real rail values (≈x3622-5484, matching the beach) get applied —
  cf. the `TParamT::load .prm BE swap` class. Once Mario's rail sits on the collision, no
  oob-kill → STATE stays 4 → `moveStage` fires → gameplay. Quick A/B sanity: if you force
  mXMin≈3622/mZ≈-700 (or skip the oob-kill in the option map) Mario should survive and the
  transition should fire — use only to confirm the diagnosis, the real fix is Option.prm.

## UPDATE 2 — the real blocker is BROKEN GROUND COLLISION (2 bugs found)
The Option.prm theory was also a red herring. `checkGround` at the EXACT centroid of a
known-valid ground triangle (4863,100,-636) returns 9999999 (NO ground). So the collision
LOOKUP is broken, not Mario's position. Two distinct bugs:

1. **FIXED — `checkGround` read the wrong grid list.** `TMapCollisionData::checkGround`
   (MapCheck.cpp) fetched `getGridRoot{14,18}(...).getRoofList()` — i.e. `unk0[1]` (roof) —
   to look up GROUND. Ground triangles are linked into `unk0[0]` (getListRoot planeType 0).
   The decomp had no `getGroundList()`; someone pasted `getRoofList()`. CONFIRMED against the
   DOL (checkGround__17TMapCollisionData 0x8018c18c reads `root+4` = `unk0[0].getNext()`).
   Fix: added `getGroundList()` (`unk0[0].getNext()`) and used it in checkGround.

2. **OPEN — most `.col` triangles load with GARBAGE vertices.** Even after fix #1, the
   centroid still finds no ground. `[gndlink]` shows the first ~2 triangles are valid
   (triX[3915..5191]) but the rest have wild coords (triX spanning -48917..60983, -3992
   recurring). So the collision vertex/index parse in the `/scene/map/map.col` loader
   (TMapCollisionBase::init BE-swap/relayout + initAllCheckData/setCheckData via the s16
   index arrays; vtxCount=392 so it takes the `thing|=4` raw-`&unk14->x` path) corrupts most
   triangles. NEXT: dump/verify the per-group index arrays (thing->unk8) and the vertex array
   after the BE swap — the indices or vertex stride go wrong after the first group. This is
   the true fix for ground collision (affects EVERY stage, not just file-select); once
   triangles load clean, checkGround finds ground → Mario survives file-select → moveStage
   fires → gameplay. (added=616 triangles total; 12000 is just the scene's pre-alloc cap.)

Diagnostics added for this: `SB_DEATH_DBG` now also drives `[mapcol]` (grid extent, tri
dump, centroid checkGround), `[gndlink]` (ground link tests vs linked), and `SB_OPT_FIX`
(force Mario onto the beach — does NOT revive him, consistent with bug #2 being lookup/data,
not position).

## RESOLVED — file-select is spatially correct + the transition fires (2026-06-24)
The GAME_OVER blocker is FULLY FIXED. It was NOT Option.prm and NOT a death plane. Two bugs:
1. **Ground collision was 100% broken** (checkGround read the roof list via getRoofList, and
   checkGroundList's not-found sentinel was 9999999 instead of -32767 so the MAX always picked
   "not found"). Both DOL-confirmed and fixed → checkGround now finds the y=100 option floor.
2. **THE KEYSTONE: TPlacement::load never byteswapped mPosition** (raw stream.read of BE floats
   → every real value like 100.0 became a ~0 denormal). EVERY scene object loaded at ~(0,0,0).
   Fixed (byteswap) → file blocks load at (840/1080/1320, 300, -1000) on Mario's rail; Mario
   loads with his real Y, settles on the y=100 floor at (950,100,-1000) instead of falling.

Verified end-to-end: Mario survives file-select, the director stays STATE_UNK4, and
SB_SEL_PICK=0 (deterministic head-butt) drives PROGRESS_UNK13 → UNK1B → UNK29 → setNextStage →
**dir-state 4 -> 9 (moveStage FIRED)**. The crash after that is the DELFINO STAGE LOAD (out of
scope per the "perfect boot->file-select before Delfino" directive). Ground truth for the disc
data: option.szs (entry 133) map.col = flat y=100 floor over the whole rail; Option.prm =
mXMin 846/mXMax 1800/mZ -1000 (rail is correct). Extractor: scratch/extract_optioncol.py.

REMAINING for "perfect file-select" (fidelity, not crashes): the 110 masked missing score-panel
panes (sh0a..sh6j / st_0..st_6 / n_Xc — surfaced by the J2DScreen fail-loud change), lingering
title-logo letters, striped beach artifact, hx_wipe type 10.

## New env-gated diagnostics (committed)
- `SB_SEL_PICK=<0|1|2>` — deterministic faithful head-butt injection (Mario is in scripted
  `waitingStart`, not freely controllable headless). Fires `unk278[idx]->pushed()` once when
  PROGRESS_UNK13 + selectBookmark `unk10==2`.
- `SB_SEL_POS` — Mario + 3 block world positions each frame (in `TCardLoad::perform`).
- `SB_PLACE_DBG` — per-placement (name + pos) trace in `TPlacement::load`.
- `SB_JPA_DBG` — JPA `createEmitterBase` resource trace.
- `SB_J3D_DBG` (existing) now also prints `unk124` in `[dir]` and `[movestage] FIRED`.

## Tooling: Ghidra-headless decompile (reusable, used for setNextStage)
Cached project `scratch/ghidra_proj/sms.rep` (program `sms_flat.bin`). Decompile any US addr:
    printf "<hexaddr>\n" > scratch/decomp_targets.txt
    DECOMP_TARGETS=$PWD/scratch/decomp_targets.txt DECOMP_OUT=$PWD/scratch/decomp \
      analyzeHeadless scratch/ghidra_proj sms \
      -process sms_flat.bin -noanalysis -scriptPath scratch -postScript CreateAndDecomp.py -okToDelete
      # (analyzeHeadless resolves via $PATH; symlink now points at current Ghidra install)
    # -> scratch/decomp/<addr>.c   (US func addrs: reference/sms_gmse01_funcs.txt;
    #    many GC2D/TCardLoad funcs are NOT in funcs.txt — use the JP symbol name in
    #    reference/sms_gmsj01_symbols.txt then find the US addr by the mangled name.)
