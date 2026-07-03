# Save-file blocks + OPTIONS sign appear — drive_chr wasn't called under SB_OWN_GXLIST (2026-07-03)

## TL;DR
The title-screen native render was missing the 3D save-file blocks (A/B/C
cubes) and the wooden OPTIONS sign. Root cause was a **plain dispatch gap**:
`drive_chr()` — the native driver that enters the file-select TFileLoadBlock
cubes (and any other actors the master perform-list drops the draw bit on
into DrawBuf ChrOpa/ChrXlu) — was only called on the `!sb_own_gxlist()`
branch of `scene_perform_dispatch()`. The `SB_OWN_GXLIST=1` branch (which
IS the recipe every user-facing script uses — `title_sbs.sh`, `run.sh`)
returned early after `drive_wave()` + `drive_sky()`, so the Chr buffer was
filled by the master perform list without ever having its draw bit set —
same dropped-draw-bit class the sky and wave already handled.

## The one-liner fix
Add a `drive_chr()` call inside the OWN_GXLIST early-return branch of
`scene_perform_dispatch` in `native/src/scene_drive.cpp`, right after
`drive_sky()`, gated on `SMS_NATIVE_PLATFORM` and `SB_NO_DRIVE_CHR`
(diagnostic opt-out).

## Evidence

### Before
`SB_BLK_PROBE=1` confirms the 3 TFileLoadBlock cubes exist and have valid
MActor + model + modelData:
```
[blkprobe] block0 state=0 primaryMActor=... model=... keeper=... actorNum=3
[blkprobe]   mactor[0]=... shapes=2 mats=2   ← the cube
```
But `SB_J3D_DBG=1` never emits `[drive-chr]` — drive_chr never runs.

### After
Native SBS (`sbs_blocks_fixed.png`) shows A / B / C cubes at their correct
positions below the "NEW/NEW/NEW" panes, AND the OPTIONS wooden sign now
draws (another Chr-buffer actor). Blocks show letters as expected. Water
still solid teal from the previous fix. Metric moved
`mean_abs_pixel_delta` 64.3 → 63.5 (−0.8); `channel_mean_delta` 5.9 → 7.8
(the added blocks are darker than what was under them, hence signed drift).

## Why the branches diverged in the first place
`SB_OWN_GXLIST` was introduced as the recipe that lets the *game's own* GX
perform list run through PerformLists.bin end-to-end (so scene batches come
from real dispatch, not hand-driven `perform(0x8)`). The sky and wave were
gated behind `SB_NATIVE_PLATFORM`-inside-OWN_GXLIST because their draw bits
are dropped by the master list; **the file-select Chr buffer has the exact
same drop**, but `drive_chr` was mistakenly left only on the non-OWN branch.
The old comment on `drive_chr` even names this class ("dropped-draw-bit
class of sky/map/chr") — the fix is trivial mechanical.

## Not fixed here (separate defects)
- **Palm tree missing** — probably a Map-buffer or scene-object actor whose
  perform doesn't dispatch under OWN_GXLIST, not a Chr member.
- **Mario animation wrong** — Mario is standing/running instead of sitting
  cross-legged (`ANIM_WAIT` vs the file-select sit anim). Separate: pose
  selection / mAnimationId 0xc3 vs the file-select-specific one.
- Residual water polish (still solid teal vs oracle's turquoise reflection).
