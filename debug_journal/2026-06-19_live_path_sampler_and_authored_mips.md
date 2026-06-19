# 2026-06-19 — Live present path: per-texture GX sampler state + authored mipmaps

Continuing the handoff (`scratch/handoff_2026-06-19_render_gaps.md`): port renderer feature gaps
faithfully and broadly; verify each renders (screenshot / no garble / no regression), do NOT chase
the wash delta. Wash resolves as coverage grows.

## Key finding — ee4b568's per-texture sampler fix landed in the WRONG (diagnostic) file
The handoff stated the prior session's `ee4b568` ("honour per-texture GX sampler state") was landed +
verified for the live render. It was NOT: `ee4b568` only modified `runtime/render/vk_mesh.cpp`, which
is the **`/ngxrender` + `/ngxpresenttest` SELFTEST** path (probe-only, diagnostic). The LIVE present
renderer is `PresentRenderer::render` in `runtime/render/ngx_present.cpp`, and it still bound ONE
global REPEAT+LINEAR sampler for every texmap (`ngx_present.cpp:1025`), ignoring the per-texture
`wrap_s/wrap_t/min_filter/mag_filter` that `NgxTexBind` already captured. The two files are
independent renderers (the live path is fully self-contained; it does not call into vk_mesh). So the
live path had TWO real texture-feature gaps, not one.

## What I ported (both in `ngx_present.cpp`, the live path)
1. **Per-texture sampler** (`sampler_for()` + `samp_cache`): one cached `VkSampler` per distinct GX
   sampler state. GX wrap 0=CLAMP→CLAMP_TO_EDGE, 1=REPEAT, 2=MIRROR→MIRRORED_REPEAT. GXTexFilter min:
   texel filter = LSB (even=NEAREST, odd=LINEAR), mip mode = LINEAR for `*_MIP_LIN` (≥4) else NEAREST;
   mag 0=NEAR/1=LINEAR. `maxLod = mip_count-1` (clamp to the authored levels) else 0. Bound per
   batch-texmap (new `bsamplers[]` parallel to `bviews[]`). The J2D/HUD path keeps its dedicated
   clamp `j2d_sampler` (HUD `NgxTexBind.mip_count=0` ⇒ single level, unchanged).
2. **Authored mipmaps** (replaces the box-generated chain): a GC TIMG stores its mip levels
   consecutively after level 0 (level k at `max(1,w>>k)×max(1,h>>k)`, tiled/block-padded). SMS AUTHORS
   its high-mip levels (the distant plaza floor's are darker than level 0), which a box filter cannot
   reproduce. `texture_for` now decodes each authored level natively (`sb_tex_decode` from
   `host + running-offset`) and uploads it to the matching VK mip level (`mips = t.mip_count`, which
   the capture already gates to >1 only when the GX min filter is a mip variant AND the TIMG stores
   >1 level). The old box-gen via `vkCmdBlitImage` is gone (it was a non-authored approximation, and
   it also wrongly mip-minified textures the game samples with a non-mip filter — global
   `maxLod=CLAMP_NONE`). Dropped the now-unneeded `TRANSFER_SRC` usage on the texture image.
   - **texcache key now includes `mip_count`**: the cached image's uploaded level count depends on it,
     so a texture decoded by the J2D path (mip_count=0 → 1 level) can't be reused by a 3D mip-sampled
     draw of the same address (which needs the authored levels), and vice-versa.
   - EFB-copy eviction frees only the image resources, NOT the shared `samp_cache` samplers — correct.

## Verification (faithful + engaged + no regression)
- `render_test` still 1/1 (10/10 internal).
- Live headless (`SUNBRIGHT_FASTBOOT=1 SKIP_THP=1 NGX_PRESENT=1`, `/loadstate freeroam_plaza.sav`):
  plaza renders faithfully — floor tiles, palm, buildings, Mario, HUD coin/FLUDD — no garble, no black
  (`scratch/screenshots/mip_after2.png`). (The magenta NPC blob top-right is the separate, known
  matColor-source bug, not touched here.)
- **Authored-mip path is genuinely exercised, not dead code**: added a permanent
  `mip_textures` counter to `/ngxpresentlive` (`sb_ngx_mip_textures()`) — plaza reports
  `mip_textures=18` of `textures_decoded=131`. (18 textures carried authored mips and were decoded
  with them.)
- **No regression**: `ab_oracle.sh scratch/freeroam_plaza.sav` = **18.3%** mean delta vs the 18.4%
  baseline (within cross-run drift). Per the directive I did NOT chase the wash; this is a coverage
  step, and it neither moved nor hurt the delta measurably.

---

# 2026-06-19 (cont.) — COLOR1A1 channel computed correctly (NOT an alias) — and it does NOT fix the wash

Gap #2 from the handoff. ngx computed only COLOR0 and the generated TEV shader hardcoded `col1 = col0`
(`tev_shader.cpp:264`), so any combiner stage reading rasChan=COLOR1/ALPHA1 (floor s4, buildings s1)
got the wrong (col0) raster. Prior ATTEMPT 1 (reverted) implemented the plumbing but fed COLOR1
**ambient=0** → whole scene black. This redo feeds COLOR1's REAL material/ambient.

## What I ported (faithful, verified vs decomp)
- **Capture** (`capture_colorchan`): COLOR1 ctrl = `mColorChan[2]` @ `chan_off+4`, ALPHA1 = `[3]` @
  `chan_off+6`; material colour = `mMatColor[1]` @ 0x08; ambient = `mAmbColor[1]` @ 0x10 (CLON; CLOF
  has no ambient field → 0). EXACTLY symmetric to COLOR0 (`mMatColor[0]@0x04`, `mAmbColor[0]@0x0C`),
  verified against `J3DColorBlocks.hpp` (mMatColor[2]@0x04, mAmbColor[2]@0x0C, mColorChan[4]@0x16 for
  LightOn / @0x0E for LightOff) and `J3DColorChan` = a u16 mChanCtrl. THE prior attempt's bug was
  ambient=0 for ALL channel-1; the real ambient is the block's own mAmbColor[1].
- **light_vertex**: computes a 2nd raster via the SAME unit-tested `ngx::light_color0` with channel-1's
  ctrl/matColor1/ambColor1 (channel-1 VTX sources read the vertex CLR1 = `v.clr[1]`). out1 alpha from
  the ALPHA1 ctrl src bit.
- **Vertex pipeline**: `NgxRenderVertex.rgba1[4]` appended (uv offsets unchanged); the main packing
  carries lit col1 through frustum clipping (VW 24→28); imm geometry sets rgba1=rgba (no distinct
  channel). Vertex attr loc10 added in BOTH the live present (`ngx_present.cpp`) and the selftest
  (`vk_mesh.cpp`). `mesh.vert.glsl`: `color1` in loc10 → `vColor1` out loc9; `tev_shader.cpp`:
  `col1 = round(vColor1*255)`.
- **CRITICAL gotcha that re-created the "black scene" symptom**: `ngx_clip.h ngx_clip_frustum_tri`
  had fixed internal buffers `bufA/bufB[9*24]` + a `if (stride>24) return 0` guard ("24 = max VW").
  With VW=28 the guard dropped EVERY triangle → 3D scene black (HUD only, textures_decoded=5,
  min_w stuck at 1e30). Bumped both to 28. (Was NOT the col1 ambient — that was correct; this was the
  clip-stride guard.) Lesson: changing VW means auditing every fixed-stride consumer of the vertex.

## Verification
- render_test 1/1. Live `freeroam_plaza.sav`: renders faithfully (floor/buildings/Mario/HUD, no
  garble, no black). `ngxshape` COLOR1A1 counter: **5,605,757 of 10,430,818 lit verts have col1≠col0**
  — the 2nd channel is genuinely distinct, so this is a real feature, NOT an inert alias / dead code.
- **No regression AND the wash did NOT move**: `ab_oracle` = **18.3%** (= the 18.4% baseline, per-region
  grid identical). ⇒ **The journal's earlier "CORRECTION — the wash IS a shading bug: ngx never
  computes COLOR1" hypothesis is FALSIFIED.** Computing COLOR1 correctly does not change the washed
  floor/buildings; the `col1=col0` alias was already ~right for THOSE materials. COLOR1 is now correct
  for the millions of verts where it differs, but it is NOT the wash. (The wash remains PARKED; do not
  re-assert col1 as its cause.)

---

## Next gaps (per handoff, unchanged order)
- COLOR1A1 channel done RIGHT (feed COLOR1's real ambient `g_amb_reg[1]`, not 0 — the reverted
  ATTEMPT 1 went black using amb=0). Indirect texturing, magenta NPC matColor source, fog (OFF in
  plaza → reachability-check first). The Delfino wash stays PARKED as a fidelity chase.
