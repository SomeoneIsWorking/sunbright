# Native engine: the player Mario now renders in Delfino Plaza gameplay (2026-06-25)

## Symptom
The `sms-boot` native engine fastboots to Delfino Plaza and renders the map (buildings,
beach, sea, parasol, stalls), but **the protagonist Mario was completely absent** — no
player figure anywhere on screen.

## Root cause (named)
Mario IS spawned and his logic runs: `gpMarioOriginal`/`gpMarioPos` are valid
(=6500,300,-3850) and update each frame. But his **model was never entered into a draw
buffer**. On the GC, `TMarDirector` sequences `TMario::entryModels` through the
`DrawBuf ChrOpa`/`ChrXlu` draw buffers via the master GX perform list; sms-boot's
data-driven perform-list dispatch drops the draw bit (the same blackbox that drops the
scene/sky/map draw bits, owned elsewhere in `scene_drive.cpp`). `drive_chr()` only entered
the 3 file-select cubes (`TFileLoadBlock`), and an old `gpMarioOriginal` "ghost" enter was
disabled (gated behind `SB_DRIVE_MARIO_GHOST`) because in the FILE-SELECT/option map it
duplicated a separate display Mario. The gameplay map has no such display Mario, so nothing
ever entered the player → invisible.

## Fix (own the path, faithful to the GC)
In `native/src/scene_drive.cpp` `drive_chr()`, after the file-block loop and before the
Chr-buffer draw, enter the gameplay player faithfully — exactly as `TMarDirector` would,
with `DrawBuf ChrOpa`/`ChrXlu` already set as the active j3dSys draw buffers:
```cpp
if (gpMarioOriginal && gpMarioOriginal->mModel && !SMS_isOptionMap() && !SB_NO_DRIVE_MARIO) {
    gpMarioOriginal->calcView(&g_graphics);     // per-view draw matrices: body + hands + cap
    gpMarioOriginal->entryModels(&g_graphics);  // mModel->perform(0x200) + hands + cap -> buffers
}
```
`b0/b1->draw()` (already present) then render the entered packets. Gated to the gameplay map
(`!SMS_isOptionMap()`) so it never touches the file-select path; `SB_NO_DRIVE_MARIO=1` opts
out for A/B. New `SB_MARIO_DBG=1` dumps pos/status/model/shapes/view-space drawMtx.

## Verified
- `scene_verts` 17907 -> **21951** (+4044 = Mario's geometry); `scene_batches` 47 -> 57
  (10 Mario batches b47..b56, material key `c539bdd263592117`, ntex=3, alpha-blended).
- Mario's view-space drawMtx = (0, -104, -723): centered, just in front of the camera.
- Mario renders with CORRECT textures (red shirt, blue overalls + yellow button, skin face,
  brown shoes — his real body atlas, NOT the garbled-white the option-map ghost produced;
  the gameplay model carries its own texture table).
- No crash, no `SB_DRAWBUF_CHECK` cycle tripwire, 45s stable run. Frame
  `scratch/frames/mario_view.png`.

## Confirmed by the user mid-session ("Mario with the water device")
The white rounded shape over Mario's shoulders (topmost batch b47, samples the white/red
bottom-right of the body atlas) is **FLUDD** (the water pack), seen from behind — the camera
sits behind Mario. From behind, top-to-bottom: FLUDD (white, highest, over the shoulders) ->
brown hair (head) -> blue/red (back of body). FLUDD's plastic is white, so it's roughly
right; it currently reads as a featureless white ball lacking its grey shading / nozzle
detail. That's a multi-texture/TEV fidelity follow-up (its 3-texture material), NOT a
missing-draw bug — FLUDD IS being drawn.

## Residual / next
- FLUDD reads as a plain white ball (material/TEV detail) — fidelity follow-up.
- The foreground pavement is still bright (separate, plausibly-faithful, low priority per the
  prior handoff).
- NPCs still excluded (`JDRNameRef.cpp` kUnimplemented) — the next big engine-extension.
