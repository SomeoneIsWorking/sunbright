# Water surface white artifacts — the title-screen sea-mirror EFB composite (2026-07-03)

## TL;DR
The title-screen native render showed a broad WHITE-WASH band in the water
region (see `scratch/screenshots/sbs_sky_clouds.png` before, and
`scratch/screenshots/sbs_water_fixed.png` after). Root cause: a full-screen
composite quad (shader key hi32 `0xeb5c8e74`, drawbuf "DrawBuf MapXlu")
whose GC intent is to sample a pre-copied EFB→TEXTURE snapshot of the sea
mirror render and blend it dreamily over the visible scene. Under
`SMS_NATIVE_PLATFORM` we do not emulate GC EFB→TEXTURE copies, so the bound
texture arrives near-black; the TEV combiner then saturates the vertex-color
input (all-1) via `GX_CS_SCALE_2`, emits `(255,255,255,~1)`, and the
`SRCALPHA / INVSRCCLR` blend overwrites the visible framebuffer with white.

Per the 2026-07-03 HARD RULE (no emulation chasing — RE the intent, port
PC-native), we do not run this composite natively. Base sea batches (b3/b9
turquoise J3D shapes) draw fine underneath.

## Evidence

### Batch-attribution dump (`SB_BATCH_DBG=-1` at settled title)
```
b12  ph1  vc=15  key=eb5c8e74c39d96b8  bm=1/4/2  rgb=1.0  drawbuf="DrawBuf MapXlu"
     efb=(nil)/(nil)  texmean0=9,9,9,9
b72  ph6  vc=15  key=eb5c8e74c39d96b8  bm=1/4/2  rgb=0.87 drawbuf="DrawBuf MapXlu"
     efb=(nil)/(nil)  texmean0=9,9,9,9
```
Both quads cover the whole screen NDC extent (`ndcX ±100k, ndcY ±80k`) so
the composite paints over everything visible.

### TEV shader (dumped as `bfrag_12.glsl`)
```
stage 1:
  rastemp = col0.rgba;                            // vertex color = (255,255,255,255)
  tevin_d = ivec4(rastemp.rgb, rastemp.a);
  prev.rgb = clamp((tevin_d.rgb << 1) + ..., 0, 255);  // 255 << 1 = 510 → saturated to 255
```
`prev.rgb` saturates BEFORE the intended multiply-by-textemp modulation has
any effect. Output is `(1,1,1,1)`.

### Blend math confirms wash
`src.rgb = (1,1,1)`, `src.a = 1`.  
`bm=1/4/2` → `out = src * srcAlpha + dst * (1 - src.rgb) = (1,1,1) + (0,0,0) = (1,1,1)`.  
Every under-pixel is replaced by pure white.

### Bisection
`SB_SKIP_KEY=eb5c8e74` → water renders as solid dark teal instead of white
wash (`scratch/screenshots/sbs_nocomp.png`). One-key skip pins the artifact
uniquely to this composite.

## Why the previous "fix" attempts didn't stick
The 2026-06-30 journal
(`fileselect_overbright_is_efb_target_structure.md`) attributed this to a
render-target STRUCTURE mismatch and prescribed the segmented render +
`snapshot_efb` + EFB-sampler rebind path (`SB_FS_COMPOSITE`). That
implementation was landed but its metric win was falsified 2026-07-03 as
signed-mean cancellation: true `mean_abs_pixel_delta` stayed at ~65
regardless. The HARD RULE now bans that whole approach — chasing
GC-EFB fidelity in the native renderer is off the table.

## The fix
`native/render/sms_boot_present.cpp`, under `SMS_NATIVE_PLATFORM`, drop
scene batches with `(shaderKey >> 32) == 0xeb5c8e74`. Alongside the sky
skip pattern (2026-07-03 `sky_bmd_offscreen.md`) this is another effect
whose GC-specific mechanism we cannot reproduce — the RE'd intent
("dreamy sea-mirror blend") is not the intent of "paint pure white over
half the frame", so not running the composite is the faithful behavior.

## Verification
- Before: water region is broad WHITE with faint teal streaks.
  `mean_abs_pixel_delta = 65.5`. Row3 signed delta all POSITIVE
  (native too bright by +40..+80 across R/G/B).
- After: water region is solid dark teal / turquoise, no white wash.
  `mean_abs_pixel_delta = 64.3` (−1.2). Row3 signed delta all NEGATIVE
  (native slightly too dark, |delta| smaller). `channel_mean_delta`
  31.4→5.9 — no longer suggests an additive overbright.
- SBS: `scratch/screenshots/sbs_water_fixed.png`.

The mean_abs_pixel_delta barely moved because it aggregates over pixels
regardless of *type* of wrongness (white-wash vs slightly-too-saturated
blue are equally distant to a pixel-mean metric). The visible defect the
user named ("white artifacts") is gone.

## Residual (deferred to future tasks — separate visual defects)
- Native water is saturated dark teal; oracle has a brighter foreground
  turquoise with visible reflection highlights. Would need a native
  reflective-water pass (not just base J3D shapes) if we want that look.
- Palm tree, save-blocks (A/B/C), sitting Mario pose — separate RE gaps
  tracked as tasks #3 / #4 in the handoff list.
