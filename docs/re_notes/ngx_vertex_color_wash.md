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

## Next steps
- Audit the per-shape col0 source for the NOT-PRESENT CLR0 case (12.7M verts) — what should
  col0 be there (matColor? persisted channel?). Likely the dominant remaining white.
- Decide unk114-vs-static-array deterministically (when is the per-view buffer valid?).
- For matsrc=REG lit shapes, verify illum vs Dolphin's lighting (don't saturate).
- Tooling added this session (in `/ngxshape`): XFMEM lighting dump, FOG dump, CLR0-class &
  matsrc histograms, biggest-map-shape vcol0, TOP-batches (col0 + tex0), floor-vert lighting
  breakdown, decoded TEX0 mean. `SUNBRIGHT_NGX_TEVDBG=tex|ras` (raw texture / raw vColor A/B).
