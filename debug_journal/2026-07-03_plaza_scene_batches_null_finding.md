# Plaza gameplay — scene batches contribute ZERO to the presented frame (2026-07-03)

## Finding
Under the plaza gameplay recipe (`SB_STAGE=1 SB_SCENARIO=0 SB_OWN_GXLIST=1`, dumped at
frame 3001), **skipping ALL 144 scene batches produces a byte-identical PPM**.

## Evidence
- Baseline (no skip): `md5(boot_3001.ppm) = de64f402c8b4d62f38de3ea0c61141a4`,
  mean|Δ| vs `scratch/oracle/plaza_gameplay_oracle.png` = **88.2**.
- `SB_SKIP_RANGE=0-143` (drop every scene batch via
  `native/render/sms_boot_present.cpp:314-321`, added this session):
  `md5(boot_3001.ppm) = de64f402c8b4d62f38de3ea0c61141a4`, mean|Δ| = **88.2**.
- Runs use `SB_TURBO=1 SB_HOST_ALLOC_CAP_MB=3072 SB_FRAME_DUMP_START=3000
  SB_FRAME_DUMP_MAX=2 SB_WATCHDOG_SECS=0`. 40s wall each.
- `SB_BATCH_DBG=3000` on the same recipe printed **144 batchdbg lines** — the batches
  ARE enumerated and reach `sms_boot_present.cpp` (`nsbatch = 144` at the settled frame).

## What this means
The observed "sparse and dark" plaza output — mean (14.3, 14.1, 9.0), 11.37% nonzero —
does NOT come from the scene batches (`sbatches[0..143]`) at all. Those 144 batches
contribute 0 pixels to the presented framebuffer under this recipe. The output must be
coming from ONE OR MORE OTHER PATHS:

1. **imm 2D pane / HUD path** (see `nvk_imm_*` + the 2D imm pipeline built for
   file-select — plaza HUD/coin/shine counters likely take this path).
2. **Prior frame residue** if `nvk_present` reuses a target that isn't cleared before
   the imm pipeline overwrites part of it.
3. **A path outside `draw_tev_segment`** entirely (e.g. `sb_native_sky_paint` or
   `sb_native_zzz_paint` gated on state — but at plaza these gates would need to
   evaluate true, unlikely).

## Falsifies handoff Task 1
The 2026-07-03 handoff pitched "SB_SKIP_KEY / SB_SKIP_BIDX batch-by-batch bisection"
as the first cut. **The scene batches were the wrong universe.** No sub-range of
[0-143] will change the presented frame either — proven by the union.

## What to try next (for a future plaza session)
1. Enumerate the imm/2D path draws at the settled frame (existing imm path already has
   `SB_SKIP_IMM` / `SB_SKIP_IMM_IDX` env — see `sms_boot_present.cpp`).
2. Snapshot the presented framebuffer BEFORE scene batches run vs after — if it's
   already at (14,14,9) before, the imm/HUD/prior-clear path owns 100%.
3. Check whether scene batches are being drawn but IMMEDIATELY overdrawn by a later
   clear or imm pane covering the whole viewport.

## Not attacking this now
User redirected (2026-07-03) to close title-screen residuals first:
- Sky top-left [+142, +88, +24] — broken/missing cloud rendering.
- Water color — dark teal vs oracle turquoise reflection.
- Any other visible-to-eye divergence.

Reason: project goal is faithful visuals, not a Δ-optimizer. Title has visible defects
that outrank plaza gameplay work.
