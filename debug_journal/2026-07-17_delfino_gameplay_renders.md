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

1. **Mario (the character) is not visible** in the plaza establishing shot. Retail shows him
   center-foreground. No `TMario` STUB fires (his code is ported), no panic. Open causes to
   check: origin/unset position (cf. file-select TPlacement mPosition BE keystone), not
   spawned for this entry, camera framing (is SB_STAGE=1 an intro establishing cam?), or he IS
   the anomalous dark band below.
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
