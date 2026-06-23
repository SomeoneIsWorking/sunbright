# 2026-06-23 — ⚠ CORRECTION: the real FILE-SELECT is gameplay STAGE 15, not TSelectDir

**USER (ground truth, observing the running game): "This is not file select. File select is
after the first THP, in the same scene as the title screen."** This overturns the project's
long-standing mislabel. Recorded here so it is NOT re-discovered.

## What each "select" screen ACTUALLY is
- **TSelectDir + `select.szs`/`scenario_select_1.blo` (APP_STATE_TITLE)** = the **SCENARIO / SHINE
  select** (pick an episode; the "EPISODE N / <stage banner> / SCORE / shine grid" screen, with
  `shine_menu.bmd`, `sc_map_*.bti`, shine-sparkle JPA). This is what every prior session +
  `SB_FILESELECT=1` called "file-select" — WRONG. It is reached in-game when choosing a scenario,
  NOT the save-file picker. (My milestone-4 stages 1+2 this session are real work on THIS screen —
  not wasted, just mislabeled. The whole GC2D/SelectDir/SelectMenu TU comments say "file-select";
  that wording is wrong and should be relabeled "scenario-select".)
- **TMenuDirector + `title.szs`/`title.blo` (APP_STATE_MENU)** = the **DEBUG STAGE-SELECT** dev menu
  (lisA/B/C lists, "ビーチ %d", movie picker). Not retail file-select either.
- **GAMEPLAY STAGE 15 + `option.arc` (`/data/option.szs` + `/data/scene/option.szs`),
  TMarDirector** = the **REAL title screen + save-file select**. Assets prove it:
  `fileloadblocka/b/c.bmd` + `file_name_a/b/c.bti` + `file_name_mario.bti` (the 3 save-file blocks,
  class `TFileLoadBlock` @0x801eef3c), `title_logo.bti` / `title_start_*.bti` ("PUSH START"),
  `select_new/copy/erase/option/start.bti` (file menu), and the shared 3D backdrop
  `map.bmd`(Delfino island)/`sky.bmd`/`sun_lensfx.bmd`. Reached NATURALLY: GC logo → DONE
  (mMovie=9 Entrance.thp) → MOVIE → mNextArea=(15,0,0) → GAMEPLAY stage 15 = "the same scene as the
  title screen, after the first THP".

## Current state (VERIFIED by frame dump, watchdog ON)
- `SB_STAGE=15 SB_SCENARIO=0` (gameplay fastboot, NOT SB_FILESELECT) → the title scene **RENDERS
  CORRECTLY**: "SUPER MARIO SUNSHINE / PRESS START! / ©2002 NINTENDO" with the sun mascot, palms,
  rainbow (`scratch/frames/real_fileselect_s15.png`). The gameplay/J3D pipeline already draws it.
- Pressing Start (`SB_PAD_SCRIPT="620:START 626:-"`) to advance into the file-select (the 3
  `fileloadblock` save files) hits a **SolidHeap OOM**: `[jkr] SolidHeap OUT OF MEMORY
  requiredSize=0x1950 mFreeSize=0x0` — the fileloadblock allocation exhausts a SolidHeap that
  isn't wired to host-malloc overflow (cf. [[host-malloc-oom-and-drawphase-keystone]] /
  sb_jkr_host_alloc, which covers ExpHeap/SolidHeap overflow — SolidHeap path here apparently
  not). So the title renders but the file-blocks sub-state can't allocate yet. Frame stays on the
  title (`scratch/frames/fileselect_after_start.png`).

## NEXT (the corrected file-select work)
1. Repoint `SB_FILESELECT` at gameplay stage 15 (or just use `SB_STAGE=15`); the
   TSelectDir/APP_STATE_TITLE path is the scenario-select, keep it but stop calling it file-select.
2. Fix the SolidHeap OOM on the Start→file-select transition (extend the host-malloc overflow to
   the SolidHeap path used by the fileloadblock load) so the 3 save-file blocks render.
3. Then own the file-select UI/logic (TFileLoadBlock, the file menu New/Copy/Erase/Start, card
   read of each file's progress) as a gameplay-scene port.
4. Relabel the GC2D/SelectDir + SelectMenu TUs + memories from "file-select" → "scenario-select".

## Repro
    pkill -9 -x sms-boot; (timeout 110 setarch -R env SUNBRIGHT_DISC=scratch/disc/sms.iso \
      SB_THP_FAST=1 SB_HOST_ALLOC_CAP_MB=3072 SB_STAGE=15 SB_SCENARIO=0 \
      SB_FRAME_DUMP=1 SB_FRAME_DUMP_START=600 SB_FRAME_DUMP_MAX=2 ./build-native/sms-boot \
      > scratch/frames/s15.log 2>&1 &); sleep 98; pkill -9 -x sms-boot   # watchdog ON
    # add SB_PAD_SCRIPT="620:START 626:-" to drive into the file-select (currently → SolidHeap OOM).
    # List the scene arcs: ./build-freshtest/sunbright-jingle scratch/disc/sms.iso --extract \
    #   /data/option.szs scratch/arc ; then tools/jingle rarc_files (see this session's commands).
