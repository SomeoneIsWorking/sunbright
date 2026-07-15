# 2026-07-15 — File-select parity vs the oracle: mostly faithful, 3 real residuals

User directive: file-select parity, compare against the oracle. Built the oracle harness,
captured both sides at FULL RES, and compared. The file-select (stage-15 save-blocks,
reached by pressing START at the title) renders the full beach/ocean/sky/palm scene + the
"Select data." UI faithfully. There ARE real residuals (below).

## ⚠️ Downscaled/`_640` captures LIED — always compare at full res

Early `_640` downscales showed a WHITE background + "faded" Mario and I nearly chased a
"missing background" defect. Both were DOWNSCALE ARTIFACTS: the full-res native
(`scratch/shots/fsel_blocks_1400.png`) clearly renders the whole beach scene, and Mario is
fine (his big white gloves are correct). A naive pixel-diff was ALSO junk (67% "different")
because native dumps 1280x960 (480 lines) and the aurora replay dumps 1280x896 (448 EFB
lines) — scaling both to a common size vertically MISALIGNS every UI element. LESSON: view
full res; for a real pixel diff, capture both sides at the SAME height first.

## The oracle harness (durable, reusable)

File-select is INPUT-GATED (needs a START press) so the deterministic field-count fifo
recorder can't reach it. Added to the Dolphin fork (now a SUBMODULE): DolphinNoGUI
**`--pad-start-at <field> --pad-start-frames <n>`** (fork 2059b5b) injects a headless GC
START over a VI-field window (CSIDevice_GCController::GetPadStatus), driven by the existing
vi_end_field counter. Working recipe:
`--pad-start-at=7300 --pad-start-frames=20 --fifo-record-after=7800 --fifo-record-frames=3`
(START ≥7300 reaches the save-blocks = ~3040 draws; 6800 was too early = settled title,
1258 draws). Replay the .dff through aurora (`SB_FIFO_REPLAY=...dff SB_SYNC_PIPELINES=1`) =
pixel oracle. Cached: `scratch/oracle/fifo/fsel_try_7300.dff`.
Native: `SB_STAGE=15 SB_PAD_SCRIPT="600:START 610:-" SB_DUMP_FRAME=... SB_DUMP_FRAME_AFTER=1400`.

## Real residuals (native vs oracle, both full-res) — the parity worklist

1. **Ocean sun-glare missing.** Oracle sea has bright white sun-glint/foam across the
   mid-distance water; native sea is a uniform saturated turquoise (no glare). = the
   reflective-sea sun-glint/additive-ripple path (TMapObjWave / TMirrorCamera / sea notes)
   not fully rendering here.
2. **Palm tree under-lit.** Native palm TRUNK is a dark near-black silhouette + fronds dark
   green; oracle trunk is light/textured + fronds lighter. Native palm is too dark
   (lighting or the trunk texture/material).
3. **A/B/C file cubes under-lit.** Native cubes are darker/dimmer brown; oracle cubes are
   brighter golden. Same darkening as the palm — likely a shared scene-lighting/ambient
   difference on the file-select 3D objects.

Not defects: file-block content (native all "New" = blank save is correct; oracle slot 1 =
shine×01 from Dolphin's memcard) and Mario anim-pose (mismatched capture instants).

## Next

Residuals 2+3 (palm + cubes both darker) smell like ONE shared cause: the file-select
scene's lighting/ambient. Diagnose that first (one fix may resolve both). Residual 1 (sea
glare) ties into the existing reflective-sea arc. For a clean quantified diff, first fix the
capture-height mismatch (dump native at 448, or the oracle at 480).
