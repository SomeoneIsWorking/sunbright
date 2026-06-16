# ngx 3D wash-out (2026-06-15) — ROOT CAUSE = missing per-vertex COLOR0 shading

## Symptom
`SUNBRIGHT_NGX_PRESENT` 3D scene is ~2× too bright / washed-out vs Dolphin GX of the
same state (matched-state A/B with `scratch/delfino.sav`). Transfer fit `GX ≈ 0.4·ngx^0.55`.
Per-region: floor ngx [238,231,217] vs GX [95,99,101]; sky ~3×; buildings ~2.8×.

## What it is NOT (ruled out with evidence, do not re-chase)
- **Present / sRGB / colorspace**: ngx renders RGBA8_UNORM and is substituted into the SAME
  XFB present+ProcessFrameDumping path as Dolphin's GX output → difference is in CONTENT.
- **Fog**: `bpmem.fog.c_proj_fsel.fsel == 0` (disabled) on Delfino. Not fog.
- **Texture decode**: faithful (matches Dolphin). Floor texture decodes to a normal bright
  tan ~(200,152,140). The textures are right.
- **A separate fullscreen/EFB darken pass**: not found; the darkening is per-vertex.

## What it IS
The static map geometry (floor/buildings/sky) carries its baked shading in the **per-vertex
COLOR0 channel** (indexed CLR0 vertex colours, e.g. a map shape with CLR0 = (33,44,100) dark
blue). GX modulates `tex × col0`, darkening it ~0.5. ngx was producing **col0 = white**, so
`tex × white = tex` (bright). This is the user's "missing shading that makes it darker".

The "NOLIGHT gives identical output" red herring (handoff): both the lit path and the
nolight path coincidentally yielded white, so toggling lighting changed nothing — that is NOT
evidence lighting is irrelevant; it's evidence col0 was stuck at white.

## Why col0 was white — multiple causes, partially fixed
1. **Indexed CLR0 array base was null** (FIXED). `J3DShape::loadVtxArray` sets CLR0 array =
   `j3dSys.unk114`, which is **null for static map geometry**; the real array is the static
   BMD one baked by `makeVtxArrayCmd` = `J3DVertexData::mVtxColorArray[0]` @ `vdata+0x1C`
   (CLR1 = [1] @ +0x20, already used). Fix in `build_cp` (ngx_j3d_shape.cpp): fall back to
   `vdata+0x1C` when `unk114` is null. After this, some shapes read dark CLR0 correctly
   (ti=9 sky col0 → blue (0.59,0.75,1.0); ti=10 floor → (0.70,0.72,0.80)).
2. **Channel-control (matsrc) was read from a wrong/global source** (FIXED). An earlier detour
   captured cc from a global `GXSetChanCtrl` function-hook (`g_gx_cc`) which read matsrc=REG for
   everything (J3D sets channel state via direct XF writes the function hook misses). The
   CORRECT source is the per-material **J3D color block** in guest RAM, verified offsets:
   `J3DColorBlockLightOff` (vtable 0x803e0d38): mMatColor[2]@0x04, mColorChanNum@0x0C,
   mColorChan[4]@0x0E (= {COLOR0,ALPHA0,COLOR1,ALPHA1}, each u16 packed GX reg), mCullMode@0x16.
   `J3DColorChan` bit layout (J3DColorChan.hpp `calcColorChanID`): bit0 matSrc, bit1 enable,
   bits2-5 lightMask0-3, bit6 ambSrc, bits7-8 diffuseFn, bits9-10 attnFn, bits11-14 lightMask4-7.
   With the block parse, matsrc histogram flipped to mostly **VTX** (66M vs reg 17M) → col0
   correctly sourced from the vertex colour.
3. **Ambient was a stale purple** (FIXED). The global GXSetChanAmbColor capture gave (128,66,~112)
   where xfmem ground truth = 0; for LightOff blocks (no stored ambient) use 0. Removed a purple
   tint + over-bright.

## STILL OPEN — the present render is still washed (flaky)
Even with the above, the PRESENT render stays ~washed and it is **flaky/scene-dependent**:
- In one run the probe shows ti=9/ti=10 col0 darkened; in another `diag_ras` (output vColor)
  shows the floor col0 = white (247). The col0 source flickers.
- `CLR0 class hist`: idx16 = ~71M verts, **notpresent = ~12.7M verts** → those default to white
  → bright. Shapes whose materials are matsrc=REG + lit can also saturate `illum` to white.
- So it's heterogeneous per-shape: (a) CLR0-present-indexed-dark (now mostly OK), (b)
  CLR0-not-present → white default (wrong — should use matColor / persisted channel), (c)
  matsrc=REG lit shapes whose illum is computed wrong/saturates, (d) `unk114` per-view buffer
  vs static array timing.

## Authoritative ground-truth sources (use these, NOT reference/sms which is incomplete)
- Lighting/channel state: Dolphin `xfmem` (VideoCommon/XFMemory.h) — `xfmem.color[2]` (LitChannel),
  `xfmem.ambColor[2]`, `xfmem.matColor[2]` (u32 RGBA, R in MSB), `xfmem.lights[8]`.
  ⚠ xfmem LAGS at J3DShape::draw (GPU thread async) — usable for slow-changing state (lights,
  ambient) but NOT for fast per-material cc. Light color in xfmem is stored A,B,G,R (reversed):
  shader R = color[3] (VertexShaderManager.cpp:230). Our GXLoadLightObjImm hook reads it
  correctly as (R,G,B) — verified L0=(80,80,80) matches xfmem (255,80,80,80) reversed.
- Per-material cc/matColor: the J3D color block (synchronous guest RAM), offsets above.
- Pixel state (fog/blend): Dolphin `bpmem` (VideoCommon/BPMemory.h).

## UPDATE (2026-06-16): CLR0 array source was wrong — fixed via Dolphin CP state
The decisive find: ngx used a DIFFERENT CLR0 array than GX. For the biggest shape,
`g_main_cp_state.array_bases[CPArray::Color0]` (Dolphin's authoritative base, set by the
game's GXSetArray and read by Dolphin's own vertex loader) = `0x00d39c40` (guest 0x80d39c40),
but our `build_cp` produced `0x80b9b600` — the STATIC authored base (bright), not the per-view
LIT colours (dark) the engine binds via `j3dSys.unk114`. Our own `unk114` read was stale/null
at capture time → wrong fallback. FIX: since `ov_j3dshape_draw` runs the real draw FIRST then
captures, `g_main_cp_state.array_bases[Color0]` is current → use it (OR 0x80000000 back on, it's
stored region-masked) with the static array as fallback. Result: sky + many buildings darkened
toward GX; overall col0 (diag_ras) 0.63→0.52 vert-weighted; floor ti=10 col0 0.7→0.25.
Verified the present DOES apply col0 (NGX_TEVDBG=ras shows the col0 structure).

STILL washed though: specific visible-floor batches (e.g. ti=81/ti=79, tex 80d3c660, multi-stage)
still have col0=white in the capture, and overall col0 ~0.72 area-weighted vs GX ~0.5. Those
batches' own CLR0 entries read white OR they're a matsrc/lighting case needing different handling.
Next: categorize the white-col0 visible batches (store per-batch matVtx/enable) and check whether
their g_main_cp_state CLR0 base / entries are genuinely white or a wrong array at their draw.

## UPDATE 2 (2026-06-16): category map → wash is reg/lit LIGHTING saturation
`SUNBRIGHT_NGX_TEVDBG=cat` (tints each batch by material category) shows the ENTIRE visible
floor + buildings + sky are **reg/lit** (matsrc=REG, lighting ON, matColor=white) → col0 =
clamp(illum). Trees/hedges are vtx/flat (green). So the dominant wash is the per-vertex
LIGHTING illum saturating to ~white on the visible surfaces.

Measured: up-facing reg-lit illum avg=0.607, **max=1.106 (saturates)**; @max ndl=0.872, amb=0.
GX floor illum ≈ 0.54 (floor 95 / tex ~180). So the AVERAGE is close to GX, but the visible
high-ndl (sun-facing) verts blow out to white. Breakdown of a saturating floor vertex:
light0 (sun, gray 0.31, Spot, attn=1) → +0.27; light1 (WHITE, pos near origin, distatt=(1,0,0)
= NO distance falloff → attn=1) → +0.87 (unattenuated). Sum 1.14 → clamps to 1.0 = white.
GX has the SAME light1/ambient/mat (verified vs xfmem; light color stored ABGR-reversed, our
GXLoadLightObjImm read matches) yet floor = 0.54. The lighting MATH matches Dolphin
(LightingShaderGen: Spot attn, Sign/Clamp diffuse, clamp lacc, mat*lacc/255). So an INPUT
differs that I could not pin from captured data — candidates: (a) the ambient REGISTER value at
the floor's actual draw (we force 0 for LightOff; xfmem snapshot=0 but laggy), (b) light1's
real contribution (is it actually masked/attenuated for the floor in GX?), (c) the normal-matrix
transform giving too-high ndl. NEEDS per-vertex GX ground truth: compare ngx light_vertex illum
to Dolphin's computed `lacc` for the SAME vertex (e.g. instrument Dolphin's vertex path, or read
back a known floor vertex's rasterized color). The cat/ras/tex TEVDBG modes + up-lit illum probe
are the instruments.

## ⭐ UPDATE 3 (2026-06-16): GROUND TRUTH overturns the lighting hypothesis — it's the TEXTURE
Added a Dolphin pixel-shader debug (`SUNBRIGHT_DBG_RASCOLOR`, PixelShaderGen.cpp: forces
`prev = colors_0`, the rasterized lit channel-0 colour) so a recomp-GX run renders GX's true
col0/illum. ⚠ Dolphin's on-DISK shader cache (`<home>/.cache/dolphin-emu/Shaders`, GFX_SHADER_CACHE=
true) serves cached binaries by UID and bypasses generator edits — MOVE IT ASIDE to test
(verified: red-output test only worked after clearing). GX col0 ground truth (scratch/screenshots/
gx_col0.png): the floor/buildings/sky col0 is **near-white (~0.77-0.85)** — i.e. GX's lighting
SATURATES too, and ngx MATCHES it. **So the lighting/illum is NOT the bug.**

The wash is the **TEXTURE term**. Per matched region (GX normal / GX col0 → implied GX texture
vs ngx raw texture from NGX_TEVDBG=tex):
  floor: GXtex≈126 vs ngxtex≈223 (1.77×); buildings 105 vs 163 (1.55×); sky 85 vs 188 (2.2×).
ngx's sampled textures are ~1.6-2.2× brighter than GX's. (Secondary: ngx col0 ~0.95 vs GX 0.77,
a ~1.2× — minor.) Since the texture DECODE math is faithful vs Dolphin, suspect: (a) sRGB/
colorspace mismatch on upload/sample (GC textures are gamma-space; if Dolphin samples them one way
and ngx another — e.g. ngx UNORM raw vs Dolphin sRGB-view, or a decode that gamma-encodes — you
get a uniform ~1.7× across ALL textures, matching the data), (b) wrong texture/mip bound, (c) a
multi-stage combiner where a 2nd (darkening) texture/konst is dropped. The uniform ~1.7× across
floor+buildings+sky most strongly fits a colorspace/decode issue. NEXT: check ngx's texture VkImage
format + sampler vs Dolphin's TextureCache (sRGB?), and decode a known floor texture to compare
its mean to both GX-implied (126) and ngx-sampled (223).

## ⭐⭐ UPDATE 4 (2026-06-16): it's the TEV COMBINER dropping a konst/scale darkening
Decomposed with ground truth (matched delfino.sav): GX normal floor=97, GX col0=196 (0.77) →
GX *effective* texture term = 97×255/196 = **126**. ngx raw texture (NGX_TEVDBG=tex) = **220**.
The texture DECODE is faithful (both engines decode the same ~220). So GX's combiner darkens the
texture by ~0.57 (126/220) that ngx's combiner does NOT apply. ngx's combiner ≈ passes the
texture (tex_eff ≈ raw 220). (ngx col0 ≈0.94 vs GX 0.77 is a minor secondary ~1.2×.)

The darkening is a **konst / scale / extra TEV stage** the ngx TEV-shader emission drops or
mis-emits. The dominant material `db9ecf` is a 5-stage **compare-mode** combiner (s0 bias=3) with
`kcolor[0]=(79,108,97)≈0.35` — exactly a darkening konst. The handoff already flagged "verify the
compare-combiner output matches Dolphin". So the bug is in `tev_shader.cpp` for the multi-stage /
compare-mode combiners (e.g. the konst multiply or the compare-mode result not feeding the next
stage's modulate). LIGHTING and TEXTURE-DECODE are confirmed correct — stop investigating those.

NEXT: dump ngx's generated GLSL for db9ecf and trace it vs Dolphin's PixelShaderGen for the same
material (or add a Dolphin debug that outputs the TEV result of a specific stage); find where the
konst/scale darkening is lost. The decisive instruments exist: SUNBRIGHT_DBG_RASCOLOR /
SUNBRIGHT_DBG_TEXCOLOR (Dolphin side), SUNBRIGHT_NGX_TEVDBG=tex|ras|cat (ngx side), /ngxshape FULL
state dump (combiner per stage + konst + tevreg).

## ⭐⭐⭐ UPDATE 5 (2026-06-16): clean decomposition — TWO causes: missing MIPMAPS + lighting saturation
Matched-state 5-way measurement of the SAME floor tile region (scratch/screenshots: m_gx,
gx_col0, ngx_norm_m, ngx_ras_m, ngx_tex2):
  GX:  normal=97  col0=196(0.77)         → impliedTexTerm = 97/0.77 = 126
  ngx: normal=223 col0=255(SAT) tex=223  → floor = tex×col0 (simple 08f8af modulate)
So the floor wash = **col0 1.3× too bright (255 vs 196) × texture 1.77× too bright (223 vs 126)**
= 2.3×. BOTH contribute (my UPDATE-3 "lighting is correct" was imprecise — ngx col0 SATURATES
to 255 where GX is 0.77).

CAUSE A — TEXTURE = **missing mipmaps**. The floor texture (ti=10) is CMPR 64×64, ngx rawmean=227
(a genuinely bright tile; decode is faithful). ngx uploads ONE mip level and samples mip0 (sampler
mipmapMode=NEAREST, no mip chain). GX samples the texture's MIP CHAIN; the floor UV tiles many
times → heavy minification → GX averages to ~126, while ngx (mip0 + bilinear + REPEAT) ALIASES
toward the bright tile faces (~227). Depth-dependent (more minified = brighter), matching the data.
FIX: decode + upload the TIMG's full mip chain (GC TIMGs store mipCount levels) and use a trilinear
sampler with proper LOD. (ngx_present.cpp make_dev_image/texture_for/sampler; capture_textures reads
only mip0 today.) This is the dominant fix.

CAUSE B — LIGHTING = col0 saturates (255) vs GX (196, 0.77). ngx illum for the sun-facing floor
verts exceeds 1.0 (light0 sun + light1 white-no-falloff). GX's illum is 0.77 (not saturated) with
the SAME lights/ambient/mat (verified). So an input/term still diverges (~1.3×) — secondary; revisit
after mipmaps. (Earlier per-vertex breakdown: up-facing reg-lit illum avg 0.607, max 1.106.)

## Next steps
- Audit the per-shape col0 source for the NOT-PRESENT CLR0 case (12.7M verts) — what should
  col0 be there (matColor? persisted channel?). Likely the dominant remaining white.
- Decide unk114-vs-static-array deterministically (when is the per-view buffer valid?).
- For matsrc=REG lit shapes, verify illum vs Dolphin's lighting (don't saturate).
- Tooling added this session (in `/ngxshape`): XFMEM lighting dump, FOG dump, CLR0-class &
  matsrc histograms, biggest-map-shape vcol0, TOP-batches (col0 + tex0), floor-vert lighting
  breakdown, decoded TEX0 mean. `SUNBRIGHT_NGX_TEVDBG=tex|ras` (raw texture / raw vColor A/B).
