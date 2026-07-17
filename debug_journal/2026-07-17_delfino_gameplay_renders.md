# Delfino Plaza gameplay RENDERS (SB_STAGE=1) — boots into gameplay, no crash (2026-07-17)

**Milestone:** `SB_STAGE=1` boots straight into **Delfino Plaza gameplay** and it renders
faithfully through Aurora GX — plaza tiles, lighthouse tower, ocean, buildings, awnings, grass,
and the full HUD (shine + coin counters, MARIO nameplate, LIFE sun meter, FLUDD WATER gauge).
Runs **stably with no crash** across multiple 60–95 s turbo runs (exit 137 = watchdog/timeout
kill, never SIGSEGV/OSPanic). This is the project's biggest render milestone — the boot chain
logo → title → file-select → **gameplay** now reaches a rendered, stable in-game scene.

Screenshots: `scratch/screenshots/delfino_s1_2026-07-17.png` (frame 1200),
`scratch/screenshots/delfino_s1_late.png` (frame 3200 — identical, static since no pad input).

Repro:
```
SB_HEADLESS=1 SB_TURBO=1 SB_STAGE=1 SB_DUMP_FRAME=scratch/screenshots/x.rgba \
  SB_DUMP_FRAME_AFTER=1200 ./build/sms-boot/sms-boot "$ROM"
```

## Note on the "gated Delfino gameplay crash"

Prior project notes treated Delfino gameplay as gated behind a crash. As of this build it does
NOT crash at SB_STAGE=1 (verified twice). Whatever crashed earlier is either fixed by
intervening work or lived on a different entry path — do not assume gameplay crashes anymore;
re-measure before trusting that gate.

## Gameplay frontier (defects to work, in rough priority)

1. **Mario (the character) is not visible** — ROOT-CAUSED to the VISIBLE flag (2026-07-17):
   - Mario IS spawned correctly: pos (6500,300,-3850), FLUDD equipped (`SB_LOG=fludd`), and
     `TMario::perform(0x200)` (the draw/entry phase) DOES run for him — verified via
     `SB_LOG=mario` (`SB_LOG_ONCE` in `MarioMain.cpp` perform).
   - But `entryModels()` (→ `mModel->perform(0x200)`, the actual model draw-entry) is gated by
     `doEntry`, which is FALSE because **`unk114 & UNK114_FLAG_VISIBLE` (0x2) is clear**
     (`unk114 = 0x410` = OCCLUSION_PROBE|UNK10, exactly VISIBLE cleared from the init value
     0x412). `MARIO_FLAG_UNK4` is NOT the cause.
   - VISIBLE is set at `MarioInit.cpp:89` and cleared only by `MarioAutodemo.cpp`'s scripted
     start/warp states — `startCommon()` clears it at warp-in; `waitingStart/warpIn/warpOut`
     restore it when the appearance completes. So **Mario is stuck in a start/appearance state
     (mStatus = 0x133f) with VISIBLE cleared and never restored** — the plaza-entry warp-in /
     start sequence isn't progressing to the point that re-shows him.
   - NEXT: decode mStatus 0x133f, find which start path (rolling/return/waiting/warp) Mario
     took and why its VISIBLE-restore / status transition doesn't fire (animation/timer/BCK
     not advancing, or a missing changePlayerStatus). Diagnostic left in: `SB_LOG=mario`.
   - The anomalous dark band (below) is a separate issue — it is NOT Mario (his model is never
     entered).
2. **Anomalous dark horizontal band** across the lower ~15 % of the screen (rows ~800–880,
   cols 79–1185, colour ~(43,50,68) blue-gray). Not present in retail's bright tiled floor —
   likely a shadow-system artifact (shadow volume / TLightDrawBuffer blob) or Mario rendered
   collapsed/black at the camera. Related to the file-select shadow arc + the stubbed shadow
   seams below.
3. **Unported plaza-population objects** (each a one-shot `[STUB-CALLED]`, worklist items):
   `TMapObjTree::perform`, `TCoverFruit::calcRootMatrix`, `TResetFruit::{initMapObj,
   makeObjAppeared}`, `TAnimalBase::{load,loadAfter}`, `TGuide::perform`, `TTalk2D2::{perform,
   loadAfter}`, `TMapObjBase::{kill,loadBeforeInit,getHitObjNumMax}`, `TMtxSwingRZCallBack`,
   `TMtxTimeLagCallBack`, `TModelWaterManager::drawShineShadowVolume` (baked sphere DL debt),
   `GXPeekARGB`.
4. Many `[plload] DROPPED` particle-emitter perform-list entries (Shift-JIS names not in the
   NameRef tree) — plaza particle effects won't dispatch.
