# 2026-06-16 — File-select (and general) ngx render wash

## Symptom
The native renderer (`SUNBRIGHT_NGX_PRESENT`) renders the **file-select** SKY (and sea)
washed near-white; the oracle (Dolphin GX) renders a vibrant blue sky.

Quantified on the static file-select (Start ×3 to skip the FMV — NOT fastboot):
- ngx full: median luma **~238**, sky region ≈ (244,250,251) — near white.
- oracle full: median **~158**, sky region ≈ (33,122,176) — deep blue.
- **Sand/ground MATCHES**: ngx (203,194,186) ≈ oracle (189,198,189). So the wash is
  the **sky+sea**, NOT a global post-process. (Earlier "whole image washed" was the eye
  being dominated by the large sky/sea area.)

## ⚠ CORRECTION of the previous session's conclusion (it was WRONG)
The prior note (and `[[ngx-render-fidelity-gap]]`) concluded the wash was a missing/uncaptured
**XF ambient** and that "the oracle's sky RASTER is blue." Both are **FALSE**, established this
session with the LIVE oracle + new probes:
- **Lighting/ambient is NOT the cause.** xfmem ground truth (`/xfhist`, runs in BOTH the ngx
  process AND the pure-Dolphin oracle, captured at each J3DShape::draw) shows the file-select
  uses ONLY: matColor=white, ambient ∈ {(0,0,0),(40,40,40),(255,255,255)}, light0 ∈
  {white, (255,80,80) warm}. **No blue anywhere** in any lighting input. So the blue sky is
  NOT from the colour channel / ambient / light colour.
- **The rasters MATCH.** Live A/B (`gp dbg ras` vs oracle `SUNBRIGHT_DBG_RASCOLOR`): ngx
  raster (221,228,238) ≈ oracle raster (219,230,241), both ~white. The earlier "oracle raster
  is blue" reading was taken against a FROZEN oracle (the `SUNBRIGHT_NGX_SHAPE` capture freezes
  Dolphin's GX XFB — see `[[gp-harness]]`).
- Setting ngx's sky col0→0 (`/ngxdbg?nolight=1`) does NOT darken the sky → col0 brightness is
  not the proximate cause either.

## The dominant sky shape ti=15 = an ALPHA-TESTED cloud layer (NOT the blue itself)
The sky is the biggest batch (`ti=15`, area≈1.2M, ~6× the next). Probed (`/ngxshape`
BIGGEST-BATCH + `/skyshader`):
- **2 TEV stages, 2 textures, ALPHA-TESTED (`pe.alpha=1`), blend NONE (opaque), ztest on.**
  - s0: ce=09fae8, **texmap1** (I4 32×32 ramp), coord=1 (SRTG src=19 → indexed by lit col0).
  - s1: ce=08a08f, **texmap0** (RGB5A3 64×64), coord=0 (tc0 matrix = IDENTITY; UVs span [0,1]).
- **DECODED both textures to PPM (`scratch/screenshots/skytex{0,1}.ppm`): they are WHITE/GRAY,
  NOT blue.** skytex0 (RGB5A3): top/bot white(255), mean (205,213,223) — a light cloud tex.
  skytex1 (I4): a black→white vertical ramp. So the **prior session's "white/black mask"
  texture note was RIGHT**; my mid-session "texture is the blue source" was WRONG.
- The combiner inputs are ALL white/gray (tex0 light, tex1 ramp, col0 white raster, konst=159
  scalar; the c0/c1/c2 tevreg are NOT referenced by these stages). So ti=15's own colour output
  is white/gray — it canNOT be the deep blue.

**∴ ti=15 is a white CLOUD/detail layer drawn OVER a blue sky background, gated by its ALPHA
TEST.** In the oracle the alpha test discards most of the layer → the blue background shows
through → blue sky. **In ngx the alpha test evidently does NOT discard** (renders the white
cloud opaque) → it covers the blue → washed white. That is the leading hypothesis for the wash.

## Fix landed this session: GX_TG_SRTG texgen (was unhandled, but NOT the wash)
`texgen_uv` previously fell back SRTG/COLOR sources (src 19/20, type 10) to the tex0 vertex
attribute — a wrong guess. Now it correctly sets `UV = (litcol0.r, litcol0.g)` and the transform
loop computes lighting (col0) BEFORE texgen. Faithful; no regression (sand matches). Did NOT fix
the wash (the wash is the alpha test, above). Kept as a correctness fix.

## STILL OPEN — the wash is a WHITE CLOUD over a BLUE BACKGROUND that ngx renders white
Established this session (all measured, not guessed):
- ti=15 cloud: alpha test = `comp0=GEQUAL ref0=128 AND comp1=LEQUAL ref1=255` (= keep α≥128,
  **discard α<128**), captured correctly. Its texture (tex0 RGB5A3) is **32% transparent**
  (alpha<128: 1312 texels / ≥128: 2784) and its RGB is **white everywhere** (top/bot row 255).
  Final TEV alpha reduces to `tex0.a` (col0.a=1.0 since alpha0=REG matA=255). So ngx SHOULD
  discard ~1/3 of the cloud, and even the kept cloud is WHITE — so ti=15 itself contributes
  only white + holes. **The blue must come from a shape BEHIND ti=15** that ngx renders white.
- So there are likely TWO things to chase:
  1. **Does ngx's ti=15 alpha test actually discard?** The discard is in the generated GLSL
     (`/skyshader`) and the params are right — but confirm it FIRES in the live pipeline (the
     blend=NONE + ztest=on path). If ngx draws ti=15 fully opaque (discard not wired into the
     VK pipeline for this material) the cloud veils everything. Watch for bilinear filtering
     smearing the RGB5A3 alpha across the opaque/transparent boundary (Dolphin may point-sample
     for an alpha-tested tex) — but that can't make it ALL white.
  2. **Find the blue BACKGROUND shape** drawn before ti=15 and figure out why ngx renders it
     white. xfmem shows no blue lighting and the sky textures are white/gray, so the blue
     background's own source must be found — candidate: a vertex-coloured gradient mesh
     (ti=11 = vtx/flat, untextured, col0=(0,0.95,0.76) cyan; the deep-blue one may be a smaller
     or earlier-drawn shape). Add a draw-ORDER probe (which shapes draw before ti=15 in the sky
     screen region) and dump their col0/tex to locate the blue.
Tooling for next time: SKY latch dumps cc/ca/lights/texgen/tc0+tc1 UV bbox + decoded texmap
PPMs (`scratch/screenshots/skytex{0,1}.ppm`) + per-texmap alpha<128/≥128 histogram; BIGGEST-
BATCH dumps the full combiner + alpha-test params. Extend with: a per-batch draw-order index,
an alpha-test PASS-FRACTION counter for ti=15, and a "render with ti=15 culled" toggle to see
the background directly.

## Tooling added this session (committed; expand them)
- `/xfhist` (`gp xfhist`): distinct (color0-cc, ambColor0, matColor0, light0) tuples observed
  at every J3DShape::draw — runs in the ORACLE too (always-on, not gated on capture) = the
  ground-truth ambient/light/material the GPU actually uses. THIS killed the ambient theory.
- `/skyshader?ce=HEX` (`gp get /skyshader`): dumps ngx's generated TEV GLSL for the material
  whose stage0 color_env matches (default the sky 0x09fae8) — audit the combiner, no drift.
- `/ngxshape` additions: **SKY latch** (per-draw cc/matColor/ambColor/lights/ndl/attn/illum/out
  + amb_reg_live + vcol0 + **texgen src/type/mtx** for the sky 0x0686 material) and
  **BIGGEST-BATCH TEV** (full combiner + all stages + kcolor/tevreg + every bound texmap's
  decoded mean) — identify and audit the dominant on-screen material directly.

## Ruled OUT (evidence)
Lighting/ambient (above), blend (NOBLEND unchanged), fog (fsel=0), copy-filter (unity),
palette/CI (sky is RGB5A3 not CI; palette path native), texture DECODE math (RGB5A3/RGB565
channel-expansion verified faithful), col0 brightness (nolight no-op on the sky), present/sRGB
(rasters match through the same present).
