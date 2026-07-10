# 2026-07-10 — Per-draw raster-state comparison: retail FIFO vs native draw-dump, world pass

Task: build a per-draw RASTER-STATE comparison (viewport, scissor, Z-mode, blend, alpha-compare,
cull) for retail's world-pass Sky Xlu dome + MapOpa vs native's equivalent draws at the stable
title, independent of the already-diagnosed projection carryover
(`2026-07-10_projection_carryover_root_cause.md`). No fixes made — diagnostic only.

## Ground truth (retail)

`tools/oracle/parse_fifo_dff.py` extended with `--raster-tsv` + a `BPRasterState` tracker (BP
0x00 genmode/cullmode, 0x20/0x21 scissor, 0x40 zmode, 0x41 cmode0/blend, 0xF3 alphacompare) and
XF-viewport decode (0x101a, `wd=2*w0 ht=-2*w1 x0=(xOrig-wd)-342 y0=(yOrig+ht)-342` — the 342 is
GX SDK's internal coordinate bias, confirmed via Dolphin's `BPFunctions.cpp` and cross-checked
against this .dff's own numbers: both the 256x256 mirror viewport and the 640x448 world viewport
reduce to origin (0,0) once the bias is removed, despite different sizes — only explicable as a
constant HW bias, not size-dependent). Bit layouts (GenMode.cullmode, ZMode, BlendMode,
AlphaTest) fetched directly from `dolphin-emu/dolphin` master `BPMemory.h`/`BPFunctions.cpp`
this session, not recalled/guessed.

Output: `scratch/oracle/fifo/title_world_pass_raster.tsv` — dome (202v, seq 5936/5937) + 3
MapOpa-anchor draws (posmtx translation ≈(305.42,-1043.36,-353.41), seq 7322/7516/7523), all
from the world-pass segment (proj diag [2.04163, 2.74748], viewport 640x448 @ origin (0,0)).

## Native side

`extern/aurora/lib/gx/command_processor.cpp`'s existing `SB_DRAW_DUMP` printf (already had
viewport/scissor/zcmp/zupd/blend-mode/color-alpha-update) extended with `cull=`, `zfunc=`, and
`acmp=[c0/r0/op/c1/r1]` (raw `g_gxState.cullMode`/`depthFunc`/`alphaCompare`). Captured via
`SB_HEADLESS=1 SB_STAGE=15 SB_DRAW_DUMP=1 SB_DRAW_DUMP_AFTER=2000` (paced, not turbo), windowed
to the first 200 draws once `VIGetRetraceCount()` clears 2000 — the same stable-title present
`2026-07-10_title_backdrop_black_verdict.md`/`..._projection_carryover_root_cause.md` used.
`tools/oracle/parse_native_raster_dump.py` (new) turns the captured `[draw-dump]` lines into
the same TSV shape (decoding GX SDK's `GXCullMode`/`GXBlendMode` enums to the same english
labels as the retail side's Dolphin-enum decode — the two use DIFFERENT numeric encodings for
cull mode specifically, NONE/BACK/FRONT/ALL vs NONE/FRONT/BACK/ALL, so raw-int comparison
would have been silently wrong).

Output: `scratch/logs/native_world_pass_raster.tsv` — 1 dome draw (202v, idx 262720) + 3
MapOpa-anchor draws (same translation match, idx 262721/262732/262739).

**Note**: `grep` without `-a` silently drops lines here (embedded Shift-JIS bytes elsewhere in
the log break its line detection under the current locale) — same gotcha the projection
journal already flagged; used `grep -a` / a Python byte-mode scan throughout.

## Field-by-field diff

**DOME** (retail seq 5936/5937 vs native idx 262720) — MATCH on viewport (0,0,640,448),
scissor (0,0)-(639,447), cull (BACK), z_test_enable(1)/z_func(LEQUAL)/z_update_enable(0),
blend_enable(1)/logic_op_enable(0)/dst_factor(3)/src_factor(1)/subtract(0), and the full
alpha-compare (ALWAYS/ALWAYS/OR, ref 0/0). **First divergent field: `color_update`** (retail
0, native 1), tied with `alpha_update` (retail 0, native 1) — same column pair, both flip
together. Retail's dome literally writes neither color nor alpha at this draw (a Z/mask-only
pass); native writes both. Also note (not one of the 6 requested fields, but visible in the
same row): retail's dome pos-matrix translation is (0,0,0) (posmtx_complete=True — genuinely
identity, not missing data) vs native's (25.75, 5.77, 4.20) — consistent with a camera-relative
skybox dome in retail vs an offset one in native.

**MAPOPA** (retail 3 samples vs native 3 samples) — these are NOT a verified 1:1 draw pairing
(MapOpa is dozens of individual map-piece sub-draws sharing one position-matrix anchor; only
the translation match confirms "same map region", not "same sub-material"). Fields that are
**uniform on both sides** (robust to the pairing ambiguity) and diverge:
- **`z_func`: retail ALWAYS (3/3) vs native LEQUAL (3/3).**
- **`blend_enable`: retail 1/BLEND (3/3) vs native 0/NONE (3/3).**
- **`color_update`: retail 0 (3/3) vs native 1 (3/3).**

`alpha_update` matches (1 both sides, 3/3). Viewport/scissor match exactly on every sample.
Cull mode and dst/src blend factors vary per-sample on BOTH sides (real per-sub-material
variation) — inconclusive without matching by texture/material index, not attempted here
(out of scope: diagnostic only, no fixes).

**Verdict, in TSV column order (first field that differs): `z_func`** for MapOpa,
**`color_update`** for the dome.

## What this does and doesn't explain

These are real, reproducible divergences — but none of them, alone, explains "nothing
rasterizes". A wrong `z_func`/disabled blend/wrong color-update changes what a covered pixel
looks like; it doesn't stop geometry from covering pixels in the first place. The dominant,
causally-established suppressor remains the **cross-frame projection carry-over**
(`2026-07-10_projection_carryover_root_cause.md`: native's Sky Xlu/MapOpa draws at this exact
window carry `proj=ORTHOGRAPHIC diag[0.0045,-0.0031]` instead of retail's
`proj=PERSPECTIVE diag[2.04163,2.74748]` — confirmed again directly in this session's own
native TSV, `proj_type=ORTHOGRAPHIC` on every one of the 4 sampled draws) — that ortho scale
maps camera-space coordinates in the thousands to NDC values in the hundreds, discarding every
vertex before rasterization regardless of any other raster-state bit. The raster-field
divergences found here are real secondary defects (worth fixing once the projection bug is
fixed and pixels actually land on screen), not the root suppressor.

## Tooling landed

- `tools/oracle/parse_fifo_dff.py --raster-tsv OUT [--frame N]` (+ `BPRasterState` class,
  BP-register bit tables sourced from Dolphin's public `BPMemory.h`/`BPFunctions.cpp`).
- `tools/oracle/parse_native_raster_dump.py <log> --out OUT` (new) — turns a `[draw-dump]`
  capture into the same TSV shape for diffing.
- `extern/aurora/lib/gx/command_processor.cpp`'s `SB_DRAW_DUMP` printf extended with
  `cull=`/`zfunc=`/`acmp=[...]` (aurora commit, superproject bump follows).
