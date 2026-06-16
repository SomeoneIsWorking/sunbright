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

## UPDATE 2 — the white sky is OPAQUE NON-ALPHA geometry (not the cloud, not the clear)
More live tests this session refined it further:
- `SUNBRIGHT_NGX_SKIPALPHA=1` (new toggle: skip alpha-tested batches in the present draw loop,
  ngx_present.cpp) — **no change to the sky.** So the white sky is NOT an alpha-tested shape
  (so NOT ti=15, the "biggest" cloud — its huge NDC bbox doesn't mean it covers the sky pixels).
- Texture filter NEAREST vs LINEAR (vk_mesh sampler) — no change. Not a filtering/alpha-smear.
- **Forced the 3D clear colour to bright RED** (ngx_present.cpp:639) — the sky stayed WHITE,
  not red. So the sky is covered by OPAQUE WHITE GEOMETRY (or an after-3D composite), NOT the
  clear colour. (Separately: ngx's clear is HARDCODED `(0.10,0.12,0.18)` instead of the game's
  GXSetCopyClear — a real bug to fix, but not THIS wash.)
- `dbg ras`/`dbg tex` DO change the sky → it is ngx's own render (not a frozen Dolphin XFB).
So: a **non-alpha shape outputs white over the whole sky**, while the oracle renders blue there.
- Tested the `white_view` (1×1 unbound-texmap fallback) hypothesis: set that fallback texel to
  bright GREEN → **sky stayed WHITE.** So the sky is NOT an unbound-texture shape. (Green barely
  appears anywhere → the fallback is rarely hit; texmap binding is broadly fine.)

So EVERY obvious GX-state source is now ruled out: lighting/ambient, the sky textures (white/
gray), the alpha cloud, the clear colour, filtering, and the white_view fallback. A white-texture
× white-raster opaque shape legitimately outputs white — yet the oracle is blue. The blue must
come from GX state ngx doesn't reproduce. Two remaining hypotheses for the NEXT session:
  (A) **ngx's J3D capture MISSES the blue sky shape.** If the blue background is drawn by a path
      other than `J3DShape::draw` (0x802e0390) — a different draw fn, J2D, or a direct-GX skybox
      — ngx never renders it, so its area shows whatever IS rendered (the white cloud/other).
      Check: count shapes ngx captures for the sky scene vs the oracle's GX draw count; look for
      a large sky/skybox draw the tee doesn't see. This is the strongest lead.
  (B) The **present composite** (J2D-over-3D, or the XFB/swapchain copy) injects the white.
NEXT TOOLING: a reliable **pixel→batch probe** (render the bid/tev_index pass to an offscreen
target and read back the exact sky pixel — the screenshot+bid sampling was unreliable: AA +
cross-launch camera drift, and tev_index decoded out of range). Point it at a sky pixel, dump
that batch's tev_index + bound texmaps + col0; if NO batch covers the sky pixel, it's (A)/(B).
Also fix the separate hardcoded-clear-color bug (use the game's GXSetCopyClear).

## ⚠️ UPDATE 4 (2026-06-16 pm) — CORRECTION: UPDATE 3's "ti=9" was the TITLE-SCREEN logo (screen mismatch)
UPDATE 3 concluded the wash is ti=9. That was measured on a **screen mismatch**: `gp pad start ×3`
advanced the ngx and oracle instances to DIFFERENT screens — ngx was still on the TITLE screen (the
glowing "SUPER MARIO SUNSHINE" logo, whose bloom IS ti=9) while the oracle was on the file-select.
ti=9 is the title-logo glow, NOT the file-select wash. Lesson: **always verify both instances show the
"Select data" menu before comparing** (stack the two screenshots and LOOK — `pad start ×5` w/ 2.5s
settle reaches it more reliably; tev_index numbers are per-frame and differ between scenes).

### On the VERIFIED file-select (both showing the menu), the wash is ti=11, the SKY GRADIENT itself
- ti=11 = a vtx-colour gradient sky mesh, **untextured**, **screen-blend (PE src=GX_BL_ONE=1,
  dst=GX_BL_INVSRCCLR=3 → out = src + bg·(1−src), brightens toward white)**, cc=0701 (vtx/flat).
- The mesh's vertex colours span a **blue→white gradient and are decoded CORRECTLY** (the clip dump
  shows real blue verts rgba≈(0.01,0.50,0.86) AND white verts ≈(0.9,0.98,0.97) — so it's not a
  uniform colour-decode error). But across the WHOLE on-screen sky ngx shows the **white** end
  (skyTop≈245,252,250) while the oracle shows **blue** (≈31,125,169). Because the blend is src=ONE
  screen, white src ⇒ white out unconditionally — so the question reduces to *why ngx's visible sky
  src is white where the oracle's is blue*.
- **Ruled out this session (verified file-select, runtime A/B toggles):** winding/cull (CCW flip or
  cull=FRONT culls ~everything to the clear → current CW/cull=BACK is correct), near-plane straddle
  overdraw (`SUNBRIGHT_NGX_NEARCULL` up to 5000 leaves the sky white; 50000 culls all to clear),
  ti=9 (title logo). The white-top verts have large w (genuinely in front), so it is NOT a near-w
  projection artifact.
- **CONFOUND (important):** the ngx and oracle instances are at **different scene/animation states** on
  the file-select — the J2D menu sits at a different position in each (the intro slide/fade is at a
  different frame). So the camera/scene may genuinely differ, and the stark whole-sky white-vs-blue may
  be partly a state-desync artifact rather than a pure render bug. **A trustworthy fix needs a
  SYNCHRONIZED same-state A/B**, which the capture-freeze (NGX_SHAPE freezes Dolphin GX) currently
  blocks. Leading hypotheses for next session, in order:
  1. **Fix the comparison first** — make NGX_SHAPE capture not freeze Dolphin's GX (so `/abshot2`
     same-process dual capture works), OR drive to a deterministic settled file-select state in both.
     Without this, every pixel comparison here is suspect.
  2. **ti=11 modelview/orientation** — the sky is a skybox-scale mesh (model pos ~91021) straddling the
     camera; if ngx shows the white half of the gradient where the oracle shows the blue half, the
     modelview/camera ngx reads for this shape differs. Compare ngx's per-shape modelview (sky-XF latch)
     against ground truth at a synchronized instant.
New runtime tooling added (no relaunch): `/ngxonly?ti=N` / `/ngxskip?ti=N` (isolate/remove a material
live), plus env `SUNBRIGHT_NGX_NEARCULL` / `FORCECULL` / `CCW` A/B toggles.

## (superseded — see UPDATE 4) UPDATE 3 (2026-06-16 pm) — "the wash is ti=9" — WRONG (title-screen mismatch)
Built a reliable **pixel→batch CPU rasterizer probe** (`/pixbatch`, `runtime/overrides/ngx_j3d_shape.cpp`)
— no AA, no readback, no cross-launch drift — plus a batch **clip-dump** (`/pixbatch?x=-999&y=<ti>`:
per-vertex clip[4], w-sign split, visible-mean rgb, PE/TEV/texmap/UV), a **sky-XF matrix latch**, and
`SUNBRIGHT_NGX_ONLYTI`/`SKIPTI` to isolate a material on-screen. These DECISIVELY corrected the diagnosis:

- **The sky shape is ti=11**, a vtx-color gradient mesh. Its vertex colors ARE blue (visible-mean
  rgb ≈ (0.21,0.58,0.89) = (54,148,227) ≈ the oracle sky). **`SUNBRIGHT_NGX_ONLYTI=11` renders the sky
  BLUE (69,142,194).** So the sky is captured AND rendered correctly — hypothesis A (uncaptured shape)
  is FALSE; the lighting/ambient/texture investigations were all chasing the wrong shape. (xfhist never
  saw blue because the sky's blue is a CLR0 **vertex** attribute, which xfhist doesn't capture — it
  only logs the lighting state. The whole "no blue anywhere" conclusion was a category error.)
- **The wash is ti=9**: a single-stage material that MODULATEs texmap0 (`80a83240`, a uniform GRAY
  ~0.48 I4 8×8 *static* texture — raw bytes 0x7a/0x7b, decodes correctly) by the white vertex colour,
  drawn at FAR depth over the sky with an **ADDITIVE blend (PE src=GX_BL_ONE=1, dst=INVSRCALPHA=5)**.
  Additive gray over the blue sky → washes it toward white. **`SUNBRIGHT_NGX_SKIPTI=9` → sky turns
  BLUE (skyTop 57,162,224 ≈ oracle 40,141,227; med 164 ≈ oracle 158).** That is the proof.
- GX draw counts match the oracle (233 vs 234, dl_prims 21079 identical) → ngx isn't missing geometry.

### The OPEN root cause (named, not yet fixed): ti=9's geometry is mis-handled near the camera plane
ti=9 (and the sky ti=11) have HUGE model coords (sky pos0 = (91021,0,0)); the modelview is a valid
rotation with a tiny translation, so eye coords are ~1e5 and the shapes **straddle the camera** — w
ranges from −5e4 to +5e4, 76% of ALL scene verts have w<0 (behind), NDC explodes (x up to 2959). On
the real GPU (oracle) this skybox-scale geometry renders fine; in ngx the additive ti=9 ends up
covering the whole screen (ONLYTI=9 = 100% coverage) and washes. Why ngx differs from the oracle given
identical recomp matrices is the open question. Leading hypotheses for next session:
  1. **Additive OVERDRAW from straddling triangles.** ti=9 alone over the dark clear = (154,173,178)
     (no saturation); over the bright scene the additive accumulates to white. The mis-projected
     straddling triangles may overdraw the sky region many times → src=ONE saturates. Test:
     near-plane CLIP (the faithful GPU behaviour ngx omits) or a near-cull A/B.
  2. **ngx's modelview/projection for ti=9 differs from the GPU's.** Compare ngx's computed clip vs
     **xfmem ground truth** (the GPU's actual posMatrix+projection) at ti=9's draw. The sky-XF latch
     shows ngx's P is a standard GX perspective (w=−ez), so suspect the modelview read timing or a
     second projection (note: `ngx_set_projection` DROPS non-perspective (ortho) sets — 1865 ortho vs
     784 persp per frame; if ti=9 is drawn under an ortho/other projection that ngx ignored, it uses
     the stale perspective → garbage). **CHECK THIS FIRST** — it's the most likely culprit.
Tooling for next time: `/pixbatch?x=NDC&y=NDC` (which batch wins a pixel, full depth order),
`/pixbatch?x=-999&y=<ti>` (clip dump + PE/TEV/texmap/UV for a tev_index), `gp ngx` SKY-XF + PROJ-sets
lines, `SUNBRIGHT_NGX_ONLYTI`/`SKIPTI=<ti>`. Also still TODO: the hardcoded 3D clear colour
(`ngx_present.cpp:639` = (0.10,0.12,0.18) instead of GXSetCopyClear) — separate, real, unfixed.

## (superseded) earlier framing — kept for the trail
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
