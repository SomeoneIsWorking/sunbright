# File-select Mario: garbled "double Mario" = drive_chr's redundant wrong-textured duplicate (2026-06-25)

## Symptom
On the settled choice-state file-select, where the oracle shows ONE clean front-centre Mario,
sms-boot showed TWO overlapping garbled white/blue figures (a "ghost Mario"), one with a detached
floating cap. (`scratch/frames/mario_settled_crop.png` — the two-figure crop.)

## Root cause (PROVEN with SB_MARIO_XF, settled choice frame)
Two distinct J3DModel instances were both drawing Mario body geometry at different world positions:
- **`mdl=4`** (model 0x…451f480) — the REAL file-select DISPLAY Mario: its OWN 4-entry texture
  table, the full material set incl. eyes/cap (pkt=12 / pkt=2 packets), at ndcX≈-0.41. Correctly
  textured. Drawn independently of drive_chr's Chr buffer (survives SB_NO_DRIVE_MARIO).
- **`mdl=59`** (model 0x…43074e8) — `gpMarioOriginal` (the GAMEPLAY Mario singleton), entered by
  `drive_chr` (commit 9a9cee7). Its texture table is the MAP's shared **59-entry** table
  (setMaterialTable), so every body texmap resolved against the building-atlas → garbled white. At
  ndcX≈-0.18. This is the SAME shared-table hazard as the beach-texture fix
  (`fileselect-perpacket-texture-table`), here applied to the wrong Mario instance.

drive_chr's Mario enter (9a9cee7) was added when the file-select Mario "looked absent," but the
real display Mario draws on its own now — so the enter became a now-redundant duplicate that ALSO
textured wrong. Exactly the **drive_map stale-duplicate class** (the map was drawn twice → z-fight;
removed once perform(0x8) drew it faithfully).

## Fix (native/src/scene_drive.cpp, drive_chr)
Remove the `gpMarioOriginal` enter. The real `mdl=4` display Mario remains, correctly textured.
Kept behind `SB_DRIVE_MARIO_GHOST=1` for A/B bisection only (was the default-on `SB_NO_DRIVE_MARIO`
opt-out, inverted). VERIFIED on the default build (`scratch/frames/mariofix_full.png` vs the
two-figure `scratch/frames/suns_fixed.png`): one correctly-textured Mario, no ghost — matches the
oracle's single Mario far better.

## Residual (separate, NOT the double-enter — do not conflate)
The surviving display Mario sits too far LEFT (ndcX≈-0.41) and low vs the oracle's front-centre
Mario (ndcX≈-0.1). That is the option-scene floor/placement issue (the display Mario's world
position), not a render or duplicate bug. Chase it via the file-select Mario's placement, not
drive_chr.

## Tooling
`SB_MARIO_XF=1` re-gated from a fixed present-frame window (270-276, which never coincided with the
~frame-1690 settled choice) to fire on `sb_camera_view_settled()` (capped 60 shape lines) — now it
actually dumps at the settled choice state. This is what made the two instances (mdl=4 vs mdl=59)
visible. Settled-capture recipe: `SB_SEL_DUMP_SETTLED=6` + START-mash pad + `6000:-` idle tail +
`timeout -s KILL 420` in the BACKGROUND (foreground hits the 2-min bash limit).
