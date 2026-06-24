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
    cd <home>/repo/sunbright
    cmake --build build-native --target sms-boot -j$(nproc)
    pkill -9 -x sms-boot; S=""; for f in $(seq 200 12 1690); do S="$S ${f}:START $((f+6)):-"; done
    for f in $(seq 1760 80 6000); do S="$S ${f}:A $((f+20)):-"; done
    timeout -s KILL 200 setarch -R env SUNBRIGHT_DISC=scratch/disc/sms.iso SB_THP_FAST=1 SB_TURBO=1 \
      SB_HOST_ALLOC_CAP_MB=3072 SB_STAGE=15 SB_SCENARIO=0 SB_SEL_DBG=1 SB_SEL_PICK=0 SB_J3D_DBG=1 \
      SB_PAD_SCRIPT="$S" ./build-native/sms-boot > scratch/frames/fs.log 2>&1
    grep -a "prog unk1C\|dir-state\|movestage\|selpick" scratch/frames/fs.log

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
      /opt/ghidra_11.0.3_PUBLIC/support/analyzeHeadless scratch/ghidra_proj sms \
      -process sms_flat.bin -noanalysis -scriptPath scratch -postScript CreateAndDecomp.py -okToDelete
    # -> scratch/decomp/<addr>.c   (US func addrs: reference/sms_gmse01_funcs.txt;
    #    many GC2D/TCardLoad funcs are NOT in funcs.txt — use the JP symbol name in
    #    reference/sms_gmsj01_symbols.txt then find the US addr by the mangled name.)
