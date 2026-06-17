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

## ★★★ UPDATE 5 (2026-06-16 pm) — THE COMPARISON WAS CONFOUNDED; use a SYNCHRONIZED save-state A/B
The whole file-select investigation compared TWO SEPARATE processes (ngx vs a live Dolphin-GX oracle)
driven by `gp pad start`. Those processes reach the file-select at DIFFERENT animation phases (the J2D
menu sits at different positions; UPDATE 4) — and even the same-named screen renders a different camera
phase. So the stark "ngx sky white vs oracle blue" was substantially a **state-desync artifact**, not a
clean 2× render bug.

**The fix for the methodology: `SUNBRIGHT_STATE=<save>` autoloads a deterministic save state** (once the
core settles, ~1500 VI fields; `SUNBRIGHT_STATE_FIELDS` overrides) in BOTH the ngx and the Dolphin-GX
oracle → a pixel-perfect SYNCHRONIZED A/B, zero desync. `tools/gp launch both SUNBRIGHT_STATE=scratch/
delfino.sav SUNBRIGHT_STATE_FIELDS=600` then `gp shot`. Result on the synchronized state (a mushroom-
house interior): **ngx med=119 vs oracle med=135 — CLOSE, ngx even slightly DARKER, NOT 2× washed.** So
the dramatic wash framing is largely the confound. The earlier `scratch/delfino.sav` matched-state A/B
(memory [[ngx-render-fidelity-gap]]) is exactly this tool — USE IT, don't pad-drive two processes.

**Residual real artifact (synchronized, confound-free):** ngx renders a big ORANGE/yellow GLOW BLOB over
the window/centre where the oracle shows blue sky + characters. The `/pixbatch` probe reports **"NO BATCH
COVERS"** that region → it is **uncaptured geometry** (drawn by a non-`J3DShape::draw` path, or all its
triangles are w≤0 / mis-projected) OR a present-composite layer. THIS is the real, tractable next lead:
find what draws the window/glow (it's not in the J3DShape capture) and tee/port it. Probe the blob with
`/pixbatch?x=NDC&y=NDC` and the clip dump; cross-check `/drawstats` (GX draws) vs the captured batch count.

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

---
## UPDATE 6 (2026-06-16, session 4) — THE WASH IS MULTI-MATRIX GEOMETRY, NOT SHADING

User correction (ground truth): "title screen and file select still render garbage." My prior
"largely a confound" conclusion was wrong as a takeaway — the screens ARE broken. Confirmed with
fresh headless captures + a settled-state oracle (`oracle_title.png`):
- **Title:** logo / sun god-rays / clouds render SHEARED + displaced; the base sky GRADIENT
  (ti=11, single-matrix `mCurrentDrawMtx`) is the ONLY correct 3D element. J2D overlay
  (©2002 text) is fine.
- **File-select:** J2D menu (boxes/text/OPTIONS) renders CORRECTLY; the 3D BACKGROUND is
  garbage colored triangles radiating from screen center.

### ROOT CAUSE (pinned, not a deduction)
The broken elements are **multi-matrix (PNMTXIDX) shapes**. ngx's `g_posmtx` (the per-vertex
position-matrix memory consumed by `transform_eye`) was **ZERO** for them — proven by the new
`/ngxshape` PNMTX-DBG latch: first multi-matrix non-sky shape, `matidx vert0=3`, selected
`posmtx[3]` = all zeros → those vertices transform by a null matrix → collapse/shear.

WHY g_posmtx was zero: ngx only hooked the IMMEDIATE matrix load `GXLoadPosMtxImm` (0x80362e0c).
J3DShapeMtxMulti (the logo/billboards/skinned shapes) loads matrices via the **INDEXED** path
`J3DSys::loadPosMtxIndx` (0x802e07b8) → `GXLoadPosMtxIndx` (0x80362e48) — AND a direct-XF-write
fast path — neither of which the immediate hook sees. (funcs: `loadMtxIndx_*` 0x802dfaac..,
`load__16J3DShapeMtxMultiCFv` 0x802dfd3c.)

### DEAD ENDS (do NOT re-walk)
1. **Ortho-projection hypothesis** (logo drawn under ortho, ngx dropped ortho projections):
   FALSE for the shear. BUT it exposed a real, separate bug — `ngx_set_projection` DROPPED all
   `type!=0` (ortho) projections (22722 ortho vs 10907 persp sets/frame). FIXED: track the
   current projection of EITHER type per-shape (scene_render.cpp publishes the squeezed guest
   matrix GX packs; ngx_j3d_shape.cpp `ngx_set_projection` stores both, sets `g_proj_type`).
   This fix is CORRECT and kept, but did not fix the shear.
2. **`g_main_cp_state.array_bases[CPArray::XF_A]`** (the indexed pos-matrix array base): LAGS —
   it's updated on Dolphin's async GPU thread AFTER the recomp runs. At the recomp
   GXLoadPosMtxIndx hook it held a STALE base (a 2D/J2D matrix array, 0.02-scale) → wrong matrix.
3. **`[J3DSYS+0x104]` (mCurrentDrawMtx) + index*48** as the array base (it IS the XF_A base set
   by J3DShape::draw's inlined `GXSetArray(GX_POS_MTX_ARRAY=21, base, stride=48)` @0x8035e4c4):
   read SYNCHRONOUSLY but gave a WRONG matrix (vert0 eye-z **+17307** = behind camera, w<0).
   The index/base mapping is off in a way I couldn't pin.
4. **`xfmem.posMatrices[(matidx+r)*4+c]`** (the FINAL resolved XF matrix memory): gave the
   CORRECT matrix for the latched shape (eye-z **-7308**, in front) → logo elements became
   recognizable. BUT xfmem LAGS (async GPU thread) so OTHER shapes read stale/wrong-object
   matrices → the file-select **radiating triangles** + title red wash. A degenerate(all-zero)
   guard did NOT help (the bad matrices are non-zero stale, not zero).

### THE REMAINING WORK (precise)
Capture each multi-matrix shape's per-vertex matrices **SYNCHRONOUSLY and CORRECTLY** at
J3DShape::draw time. Candidate: the fork's `LoadIndexedXF` seam (`sb_slot_xf_indexed`,
XFStructs.cpp:302) — Dolphin resolves the correct base there (that's why the oracle is right) and
`LoadIndexedXF(array, index, address, size)` HAS the XF dest `address` (slot = address/4). Extend
that hook signature to pass `address`, capture array==12 loads into `g_posmtx[address/4]`. Open
risk: it runs on Dolphin's GPU thread — must confirm the NGX path processes the FIFO
synchronously w.r.t. the recomp J3DShape::draw capture (else 1-frame lag / data race on g_posmtx;
double-buffer if needed). Also handle the direct-XF-write matrix path (`sb_slot_xf_reg`).

Current tree state (UNCOMMITTED WIP): projection fix (KEEP), PNMTX-DBG + PosMtxIndx-hook
diagnostics (KEEP — durable), xfmem multi-matrix read with degenerate guard (REGRESSES
file-select to radiating triangles — needs the synchronous source above before it's correct).

---

## 2026-06-17 (session 4) — ti=10 cloud isolated; per-pixel render diverges with ALL inputs faithful

Continued from `scratch/handoff_2026-06-17_cloud_wash_narrowed.md`. Goal: root-cause the
ti=10 additive-cloud wash. **Decisively isolated ti=10 on BOTH engines** via lockstep
`/ngxdrawlimit?n=18` vs `n=19` (shape #18 = ti=10), WITHOUT freeze (freeze pins the published
snapshot so drawlimit no-ops). Tooling: `scratch/fs_ti10lockstep.sh` (CLIPENV/TAG params),
`fs_ti10cover.sh`, `fs_uvviz.sh` (TEVDBG via `/ngxdbg?m=`), `fs_shape10.sh`.

### What ti=10 IS (sh=80e84c58, nv=40, cc COLOR0=0700 ALPHA0=0701, tex0=80a83220 8x8 I4)
A coarse **40-vertex camera-enclosing sky DOME** (fan: apex overhead at eye=(-2,39872,-3200)
→ ndc.y +34 off-top, **alpha=0**; ring at the horizon ndc.y≈-0.232, **alpha=1**). Combiner
(verified via `/gxstate?ti=10`): COLOR=`clamp(2·TEX0·TEX1·RASC)`, ALPHA=`2·TEX0a·TEX1a·RASA`,
two octaves (tc0 src=TEX0 scale-2, tc1 src=TEX1 scale-0.5, same noise tex). Blend SRC_ALPHA/ONE.
So on-screen additive = `clamp(2·t0·t1) · clamp(2·t0·t1·RASA)` (RASA=1 on the ring → `clamp(2t0t1)²`).
The sky BASE ti=11 (sh=80e84cd4, nv=752) is the SAME dome topology but UN-textured (flat blue
vtx-color) and renders FINE — so the dome geometry/apex-near-plane is not itself the bug.

### Pixel evidence (lockstep n18→n19, `scratch/screenshots/overlay_rg.png`)
- **GX cloud**: compact, FINE-detailed puff, centroid (584,195) = entirely RIGHT+lower, faint
  (+6/255 peak, ~8800 sky px).
- **ngx cloud**: big soft black/white BLOCKS, centroid (235,123) = LEFT+upper, bright
  (+50/255, ~65000 sky px) — the milky wash.
- Same shape #18, **non-overlapping screen regions**, ngx ~9× brighter + ~7× more coverage.

### RULED OUT this session (do NOT re-chase — each verified)
- **Projection**: `/ngxproj` = ngx perspective matches Dolphin's VertexShaderManager EXACTLY
  (max elem delta 0.00000). 3D-tri coverage 84.6%. So geometry projects identically.
- **Clip**: NOCLIP / NEARONLY / NEARCULL=1 ALL give identical cloud coverage (65110/65084/65103).
- **Cull/winding**: a RED HERRING. `SUNBRIGHT_NGX_CCW=1` → 111 px (clean black sky, wash gone)
  but ALSO removes the legit cloud; CW≈nocull≈65000. For a camera-INSIDE sky dome, CW (show whole
  inside) is correct; CCW just deletes the dome. cull=2=GX_CULL_BACK is correctly authored
  (J3DMaterial.cpp:807 casts mColorBlock cull byte straight to GXCullMode) and applied.
- **texgen matrix**: scale-2 is CORRECT — J3DTexMtx::load (J3DTevs.cpp:360) loads mTotalMtx@+0x64
  (exactly what ngx reads) to XF row id*3+0x1e. xfmem's "identity" is the lagged value
  ([[xfmem-not-cpu-oracle]]). NOTE: the `/texmtxloads` tee reads the SAME +0x64 ngx does, so it's
  NOT independent validation — but J3DTexMtx::load confirms +0x64 is the loaded field. So OK.
- **texcoord source**: J3DShape::loadVtxArray (J3DShape.cpp:213) overrides ONLY POS/NRM/CLR0 with
  j3dSys per-view buffers; TEXCOORD uses the static array (vdata+0x24) = what ngx reads. Correct.
- **texcoord format/frac decode**: standard VAT bit layout (ngx_vertex.cpp tex_frac etc).
- **I4 texture decode**: ngx sets texel ALPHA = intensity (`c4to8(v)*0x01010101`), so the
  `(t0·t1)²` squaring path is intact. Fragment shader DOES output combiner alpha as o.a
  (tev_shader.cpp:271 `o = clamp(vec4(prev)/255)`).
- **per-vertex alpha (RASA)**: ngx reads ring=255/apex=0 from CLR0 (cls=3 fmt=5=RGBA8, base
  80a80e40, mean a191 = 30×255 + 10×0). ALPHA0 ctrl 0701 = matsrc=VTX lighting-off → RASA = vtx
  CLR0 alpha (light_vertex line 605 honors this). col0 lighting-off → RASC=white.
- **blend**: present path reads PE block → SRC_ALPHA/ONE correctly (ngx_present.cpp:463).
- **depth**: z_test/func/write applied from PE block; GC z→Vulkan maps near=0/far=1 correctly.

### TEVDBG viz (`/ngxonly?ti=10` + `/ngxdbg?m=`, `scratch/screenshots/uv_*.png`)
- m=1 (tex0 raw): FINE detail blobs across the whole upper sky (octave-1 tiles richly). OK-looking.
- m=5 (tex1@uv1 raw): ONE big soft white blob (octave-2 low-freq). uv1 (m=7) = smooth ~1-tile
  gradient. Plausible for scale-0.5.
- Final = `clamp(2t0t1)²` saturates broadly → washed blocks.

### THE OPEN CONTRADICTION (the frontier)
Projection matches, geometry shared, every per-material INPUT faithful — yet ti=10's per-pixel
additive is ~9× brighter and covers the whole upper sky in ngx vs GX's faint localized
right-side cloud. Since math says identical inputs ⇒ identical output, ONE "verified" must
actually differ. Prime remaining suspects (UNVERIFIED, in priority order):
1. **The s1 combiner SCALE (<<1 / ×2)** — get the cloud's EXACT generated GLSL (extend
   sb_ngx_gen_shader / `/skyshader` to match by tev_index or tex0 addr) and confirm the ×2 and
   clamp. A wrong scale = 4-16× brightness, the leading explanation for the 9×.
2. **Per-pixel UV vs GX** — the apex (w=3174) vs ring (w=148000) extreme w-ratio makes
   perspective-correct UV/alpha "stick" near the apex value. Both engines SHOULD do this
   identically; if GX clips the off-top apex triangles differently the visible w-range (hence
   UV frequency + alpha) differs. Need GX's actual per-pixel UV/alpha (xfmem lags — use a
   synchronous tee or a CPU reference raster from the dumped verts).
3. **Whether ngx's #18 == GX's #18 geometrically** — lockstep gates by draw-call count; if the
   counters differ, the comparison is invalid. Cross-check the shape pointer on both sides.

Diagnostics built/used: `/ngxdrawlimit` lockstep, `/ngxverts`, `/gxstate?ti=`, `/ngxshapes`
(per-shape model pos / w-range), `/ngxdbg?m=` (TEVDBG live), `/ngxonly`, `/abshot2`, `/ngxproj`.
Scripts in `scratch/fs_ti10*.sh`, `fs_uvviz.sh`, `fs_shape10.sh`, `fs_ngxproj.sh`.

### Update (same session, later) — texcoord arrays + texture decode + fog ALSO ruled out
Added `/gxstate` dumps for tc0/tc1 vertex-array params + GX FOG (bpmem). Findings:
- **Both texcoord arrays PRESENT and distinct**: tc0 cls=3 base=80a80e80, tc1 cls=3 base=80a80fe0,
  both S16 (type=3) frac=8. So the 2nd octave (tc1=TEX1) reads real, separate data — NOT garbage.
- **Texture decode PARITY-OK**: `/tex` self-test 119/119 cases match Dolphin's oracle decoder.
- **FOG**: `/gxstate` shows fsel=0 (OFF) — BUT bpmem is GP-side and LAGS in the ngx process (same
  trap as xfmem), so this is NOT authoritative. Argument against fog anyway: fog hits ALL geometry,
  and the opaque far sky base (ti=11, w→90000) MATCHES GX — if ngx skipped real fog the sky base
  would mismatch too. To settle definitively, tee GXSetFog (synchronous) — not yet done.

### THE CONTRADICTION IS COMPLETE (and the integer math is exact)
Hand-ran the faithful GC integer combiner for mean texel 122 (`textemp=122`, RASA=255 ring):
s0→CPREV=122, s1 COLOR `((122·123)<<1+128)>>8`=117 (clamp); alpha likewise 117. Blend SRC_ALPHA/ONE
→ out = 117·(117/255) = **+54**. That MATCHES ngx's measured ~+50. GX measures **+6**. Every input
to this number is verified identical between engines. So either (a) ngx OVERDRAWS the cloud ~9×
(but it's captured once, drawn once in the lockstep increment), or (b) GX applies a darkening stage
ngx skips that is NOT fog/blend/combiner (EFB-copy intensity/gamma? a per-draw GXSetBlendMode/
GXSetTevColor override the material DL's PE block doesn't carry?), or (c) the seawash/lockstep A/B
is comparing subtly different things. The 9× ratio is suspiciously clean (= overdraw-9 or a ×8-ish
scale), worth chasing.

### NEXT STEP (decisive) — CPU reference rasterizer
Build a Dolphin-free CPU raster of ti=10 from the dumped data (40 verts: clip xyzw via ndc+cw,
uv0+uv1, alpha; the decoded 8×8 I4 texture; the known combiner; perspective-correct interp; the
GC near/frustum clip; SRC_ALPHA/ONE accumulate). Compare the reference image to BOTH ngx's render
and GX's render (`/abshot2` PPMs):
  - reference ≈ GX (faint/localized), ngx ≠ ref  ⇒ ngx's GPU/shader/present path is the bug
    (overdraw, a scale, or interp), NOT the inputs.
  - reference ≈ ngx (washed), GX ≠ ref            ⇒ GX applies an extra darkening stage (fog via
    synchronous GXSetFog tee / EFB-copy intensity) OR the dumped verts/topology are wrong.
Blocker: need the primitive TOPOLOGY (the 40-vert pattern: apex recurs every 4, positions repeat
at idx 5,9,13… — likely QUADS or a STRIP; pull the actual op from the display list) and uv1 per
vertex (extend `/ngxverts` to dump uv1). Also worth: a synchronous GXSetFog/GXSetBlendMode tee to
get the AUTHORITATIVE fog/blend (bpmem lags in-process).

### Update 2 — OVERDRAW and FOG both conclusively ruled out (cheap tests done)
- **Overdraw RULED OUT**: `SUNBRIGHT_NGX_CLOUDCOUNT=10` (new) → the present draws ti=10 exactly
  ONCE per frame (`[cloudcount] ti=10 DRAWN batches=1 verts=30`). So one faithful ngx draw = +50,
  one GX draw = +6. Not accumulation. (Note: present/cloudcount only runs under /abshot2 in headless.)
- **FOG RULED OUT (authoritative)**: new SYNCHRONOUS tee on GXSetFog @0x80361b20 → type=0
  (GX_FOG_NONE), 18157 calls. NOT the lagged bpmem — the real value. Fog is off.

So fog/overdraw/blend/combiner/texture/UV/alpha/projection/clip/cull/depth are ALL eliminated.
The 9× (ngx +50 vs GX +6, one draw each, same inputs) now points hard at either an EFB-copy
intensity/gamma stage GX applies (file-select renders 3D offscreen → copies to XFB), or a per-draw
GX register override (GXSetTevColor/GXSetBlendMode) the material-DL PE block doesn't carry, or the
seawash/lockstep A/B comparing subtly different geometry. CPU reference rasterizer remains the
decisive test. New tees committed: GXSetFog (g_fog_*), CLOUDCOUNT, /gxstate tc0/tc1+fog.

---
## 2026-06-18 (session 5) — CPU REF rasterizer: ngx renders inputs FAITHFULLY; GX ~10× fainter

Built the decisive CPU reference rasterizer (`scratch/cloud_raster.py`) the handoff asked
for. It rasterizes ti=10 from ngx's DUMPED inputs (40 verts clip-xyzw + RASA + uv0 + uv1,
exact 8×8 I4 texture decoded from 0x80a83220, faithful integer combiner from the generated
GLSL, near-clip + perspective-correct interp, quad topology). Data dumped via extended
`/ngxverts` (now raw `clip[4]`) + `/r` (texture bytes) + `/gxstate` (GLSL).

### Headline result (upper-sky cloud band, threshold >4)
| source | coverage | mean(all upper) | mean(cloud px) |
|---|---|---|---|
| GX (lockstep n18→n19) | 3509 px | **2.0** | 72.8 |
| REF faithful (linear) | 30434 px | 23.2 | 97.8 |
| REF faithful (nearest)| 16066 px | 26.0 | 208 |
| ngx | 55703 px | 39.6 | 90.5 |

**The faithful combiner from ngx's captured inputs produces the WASH (REF ≈ ngx in kind +
position, both upper), NOT GX's faint localized cloud.** So the wash is **NOT an ngx
render-math bug** — an independent faithful rasterizer reproduces it. GX's cloud is ~10×
LESS total additive energy (mean_all 2.0 vs REF 23) though per-pixel-where-present is only
~1.4× different. Neither linear nor nearest sampling in REF approaches GX.

### Ruled out this session (each verified)
- **texgen**: scale-2 is DL-baked (decomp: J3DGDLoadTexMtxImm writes the material GD DL via
  J3DGDWriteXFCmdHdr, replayed by FIFO every frame; J3DTexGenBlockBasic::load/patch).
  The J3DGDLoadTexMtxImm fn-tee + xfmem both showed IDENTITY = the **xfmem-lag trap** (tee
  misses DL replays). An earlier "live-XF identity" fix was WRONG; reverted. ngx scale-2 = GX.
- **combiner SHADER**: dumped the actual generated fragment GLSL (`/gxstate?ti=10`) — faithful
  (stage0 prev=TEX0·white=t; stage1 clamp(2·t0·t1); alpha same; blend SRC_ALPHA/ONE).
- **alpha test**: cloud is PEFL with mAlphaCmpID=0xFFFF → emits NO GDSetAlphaCompare →
  INHERITS prior GX state. Verified it inherits ALWAYS (no discard). Tried tracking+applying
  GX alpha-compare inheritance (J3DPEBlock*::load: OP/XL=ALWAYS, ED=GEQUAL 0x80, FL=its own
  or inherit) → cloud unaffected (inherits ALWAYS). Reverted. NOT the cause.
- filtering (nearest/linear/mips), fog, overdraw, blend, depth, cull, projection, clip — all
  ruled out prior + reconfirmed.

### Frontier (precise) — the ~10× attenuation
GX adds ~10× less than the faithful combiner from ngx's captured inputs. With combiner/blend/
fog/alpha all faithful, the ~10× must be an INPUT ngx captures wrong. **PRIME SUSPECT: the
per-vertex CLR0 ALPHA (RASA).** ngx reads ring alpha = 255 (1.0); the blend is SRC_ALPHA/ONE
so added = color·alpha. If GX's real ring vertex alpha is ~25 (0.1), the cloud is ~10×
fainter = GX. ngx reads RASA from vcol0[3] (CLR0, ca=0x0701 matsrc=VTX, line 651) via the
j3dSys per-view CLR0 buffer (unk114), fmt cls=3=RGBA8.
  NEXT TEST: dump the cloud's RAW CLR0 buffer bytes (j3dSys+0x114 base, per-vertex stride) and
  verify the alpha decode against the ACTUAL attribute format/frac — is ring alpha really 0xFF,
  or does ngx mis-read the alpha channel (wrong CLR0 format / component / it's RGBA6/RGB5A3)?
  Also re-confirm the GX lockstep #18 truly is the cloud (it gates the same J3DShape::draw idx,
  but the GX iso had a bottom water-glint; upper-band excludes it).
Tools: scratch/cloud_raster.py (REF), /ngxverts (raw clip[4]+uv0+uv1), /gxstate (GLSL+PE+texgen),
/texat /r (texture), SUNBRIGHT_NGX_TEXNEAREST.

### Mechanism tests in REF (none reproduce GX cleanly)
- **RASA scale**: REF with vertex-alpha ×0.1 → mean_all 2.5 (≈GX 2.0) but cov stays 20k
  (GX 3509) and mean_cloud 14.9 (GX 72.8). A uniform fade ≠ GX (GX is sparse, not dim).
- **alpha-test discard** (GEQUAL on combiner alpha): ref~200 → cov 3826 (≈GX 3509) but
  survivors mean 222 (GX 72.8 — far dimmer). A pure discard ≠ GX (GX survivors are moderate).
GX's signature = FEW pixels (3.5k) at MODERATE brightness (72.8). No single tested stage
(fade / discard / sampling) reproduces "few + moderate". REF is also ~1.3× too bright
everywhere (mean_cloud 97.8 vs 72.8) even at no discard.

### Caveat to re-check FIRST next session
The GX 3509/2.0/72.8 numbers are from ONE lockstep capture (n18→n19) whose GX iso also had a
bottom water-glint. Before trusting "GX is sparse+moderate", RE-VALIDATE the GX cloud
measurement: (a) confirm GX's draw #18 is the dome (cross-check sh ptr / vert count both
sides), (b) capture multiple frames, (c) consider that ngx's 3× supersample+downsample fills
gaps that GX's EFB resolve also does — compare at matched resolution. The "contradiction"
may partly be a measurement artifact; the CPU REF (deterministic, 1×) is the trustworthy
faithful baseline and it ≈ ngx, so ngx's render is sound.
