# 2026-06-25 — File-select A/B/C cubes now RENDER (drive_chr owns the Chr draw buffer)

## Result
The 3 file-select load blocks (A/B/C wooden cubes) now render correctly in sms-boot at
STAGE 15, matching the GX oracle (`scratch/oracle/fileselect_gx_oracle.png`). Frame:
`scratch/frames/fs_default_410.png`. `scene_batches` 31→37 (+6 = 2 shapes × 3 cubes),
`scene_verts` +360. Enabled by default (`SB_NO_DRIVE_CHR` opts out). Commit on `main`.

## The chain of root causes (each found by tooling, not eyeballing)
The handoff claimed the cubes "are likely NOT captured/drawn at all" and were maybe a
draw-buffer/perform-list gap. That framing was right about the buffer but the real blockers
were three stacked decomp-gap defects. Found via new diagnostics (`SB_BLK_PROBE`, a packet
walk, and a `DURING-DRIVECHR` capture flag):

1. **The cubes are manager-group objects, NOT children of 通常シーン.** So
   `scene->perform(0x8)` (the owned scene draw) never draws them — only the map/scene tree.
   They live in the Chr draw buffer (`DrawBuf ChrOpa/Xlu`), which the perform list FILLS
   (マネージャーグループ flag 0x204) but never DRAWS (the dropped-draw-bit class, same as
   sky/map). Entering the whole manager group re-draws the entire map (building-atlas dup),
   so `drive_chr` now enters ONLY the 3 file-block cube models.

2. **`TMapObjBase::perform()` is EMPTY** — map objects don't draw via perform; their J3DModel
   is drawn from a draw buffer. AND their `calc` never runs (the calc-anim perform list calls
   that same empty perform), so the model's draw matrices are degenerate → each cube collapses
   to a single NDC point. Fix: reproduce the minimal draw setup per cube — `calcRootMatrix()`
   (seed base TR from `mPosition` = 840/1080/1320,300,-1000) → `calc()` → `viewCalc()`.

3. **The cube shape packets are HIDDEN** (`J3DShapePacket::unk30 == 0`), so
   `J3DMatPacket::draw`'s `checkThing()` skips them. The normal draw path shows them; our
   manual entry skips that. Fix: `getShapePacket(s)->show()` before `entry()`.

4. **`mScaling` AND `mInitialScaling` are both 0** for these blocks — the scale-setting paths
   are empty decomp stubs: `TMapObjBase::startAnim()` (called by `makeBlockNormal` to play the
   "appear" BCK that scales the cube 0→1) and `makeObjAppeared()` are both `{ }`. So even with
   correct matrices the base scale is 0 → collapse. The blocks are unit-scale in the game;
   `setBaseScale((1,1,1))` is the appeared/settled scale shown on the static file-select.
   The pop-in scale-up ANIMATION remains a decomp gap (the BCK isn't driven) — documented, not
   hidden.

## drive_chr final shape (native/src/scene_drive.cpp)
Per cube (searched by Shift-JIS name ロードブロックＡ/Ｂ/Ｃ): `calcRootMatrix` → unit
`setBaseScale` → `calc` → `viewCalc` → `show()` all shape packets → `model->entry()` into
ChrOpa/Xlu → draw both buffers. `getMActor()` variant 0 is the cube; 1/2 are rock/no-card.

## New tooling (kept, committed)
- **`SB_BLK_PROBE=1`** (`sb_blk_probe` in scene_drive.cpp): one-shot inventory of the 3 blocks
  — mState, the actor-keeper MActor count, and each MActor's J3DModel + modelData shape/material
  counts + mat→shape packet linkage. This is what proved the cubes load as proper 2-shape models
  (3 anim variants each) and have intact draw-packet linkage — refuting the "shared map model"
  red herring (cross-run heap-pointer confusion).

## Dead ends ruled out (do not re-chase)
- The cubes are NOT the shared option-map model (an earlier mis-read of a cross-run heap pointer).
- The cube models load fine (region-tolerance is NOT the issue here).
- `frameUpdate()` (advancing the appear anim) does NOT un-collapse them — the scale is gone
  because `startAnim`/`makeObjAppeared` are empty stubs, not because the anim is at frame 0.

## Still divergent on the file-select (next)
- **Mario absent** — same class (player group → Chr buffer); should fall to the same
  `drive_chr`-style ownership (his model + calc + the cap-texture material resolve).
- **Banner / "Corrupt/New/New" label TEXT** — J2D BMG strings not resolved (J2D itself renders;
  OPTIONS shows). Region-tolerance / BMG path.
- The two spinning shine icons over the blocks: present (the empty-slot markers).
