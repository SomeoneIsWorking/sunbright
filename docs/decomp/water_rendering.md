# Water rendering — RE notes (GMSE01)

Status: RE complete for the screen-texture refraction stack (the user-visible "all water
effects" class). Port: `runtime/overrides/water_native.cpp` (supersedes the draft
`water_widescreen.cpp`, now a tombstone). Verification key at the bottom.

Ground truth used: `./build/sunbright-recomp --disasm` on the named addresses +
`reference/sms` decomp + `reference/sms_gmse01_funcs.txt`. Each claim is tagged
**[disasm]**, **[decomp]**, or **UNVERIFIED**.

## 1. The screen capture the water samples (TScreenTexture)

- `TScreenTexture` ("スクリーンテクスチャ", `MarioUtil/ScreenUtil.cpp`) owns a
  `JUTTexture(renderW/2, renderH/2, GX_TF_RGB565)` — a HALF-RES RGB565 copy of the frame.
  **[decomp** `TScreenTexture::load`**]** `replace` 0x8022d360 / `load` 0x8022d474 are in the
  symbol map.
- It is filled mid-frame by the normal-scene `JDrama::TEfbCtrlTex` ("通常シーン描画ステージ"):
  the bracket protocol (perform 0x80 opens, 0x8 closes with `GXSetTexCopySrc(rect)` +
  `GXCopyTex`) is documented in `runtime/overrides/efbtex_widescreen.cpp` /
  `docs/widescreen_effects.md`. The capture src rect is **(0, 0, renderW, renderH) — the FULL
  EFB** (MarDirectorSetupObjects.cpp), copied half-res. Verified live earlier (the Delfino
  edge-smear investigation, widescreen_effects.md "Dead ends"). So under the anamorphic
  widescreen scheme the texture holds the squeezed 16:9 frame; under 4:3 it holds the 4:3
  frame. The CAPTURE itself needs no aspect handling — only the LOOKUP matrices below do.
- The goop/pollution layer (`TEfbCtrlTex` graffiti passes) is a SEPARATE EFB pass class that
  renders pollution maps in texture-pixel orthos — already handled
  (`efbtex_widescreen.cpp`, squeeze suspend). Water interacts with it only as scene
  content: polluted-sea surfaces ("riccoSeaPollutionS*", "BiaWaterPollution",
  MapStaticObject.cpp actor table) are TMapStaticObj instances whose materials use the
  pollution layer textures, drawn under the normal scene projection. No extra seam.

## 2. The sea (TSea ≙ TMapStaticObj "SeaIndirect"/"sea") draw chain

SMS has no TSea class; the sea is `TMapStaticObj` (Map/MapStaticObject.cpp) instantiated
from the actor table: entry "SeaIndirect" has flags `unk40 = 0x41` (bit0 = screen-texture
indirect material, bit6 = draw-buffer swap), entry "sea" has `0x80`. **[decomp]**

- **Init** (`TMapStaticObj::init`): when `unk40 & 1`, searches "スクリーンテクスチャ", takes
  its `ResTIMG`, and `SMS_ChangeTextureAll(modelData, "indirectdummy", img)` — every material
  texture named **"indirectdummy"** in the sea BMD is replaced by the live screen texture
  (plus a direct `setResTIMG(1, img)`). **[decomp** MapStaticObject.cpp:343-349**]**
- **Per frame** (`TMapStaticObj::perform`, binary 0x80196614 — name not in the symbol map;
  identified by decomp shape + the verified `bl 0x8022ba74` at 0x8019674c **[disasm]**):
  when `param_1 & 4` and `unk40 & 1`:
  ```
  SMS_GetLightPerspectiveForEffectMtx(m);
  model->modelData->getMaterialNodePointer(0)->getTexGenBlock()->getTexMtx(1)->setEffectMtx(m);
  ```
  i.e. the screen-texture UV lookup is a J3D **effect matrix** on texmtx 1 of material 0,
  rebuilt EVERY frame from the camera. J3D consumes it as a projective texgen from POS
  (SRT/effect-mtx path inside J3DTexGenBlock — J3D internals not re-derived here; the seam
  for the port is the helper itself). The model is then drawn by `unk70->perform`
  (MActor → J3D) inside the normal scene draw; for `unk40 & 0x80` ("sea") with the
  map-group draw buffers swapped in. **[decomp]**
- The indirect-texture **wave warp** of the sea lives in the BMD material itself
  (indirect stages baked in the material data, sampling the wave texture and offsetting the
  screen-texture coordinate). It is data, not code — no code seam, and it is
  aspect-independent (small UV offsets). UNVERIFIED at the bit level (would need BMD
  material dump); irrelevant to the port since we do not touch the material.

### SMS_GetLightPerspectiveForEffectMtx 0x8022ba74 — full disasm derivation **[disasm]**

```
lwz   r4, -0x7118(r13)      ; gpCamera (SDA)
lfs   f4, 0x2C(r4)          ; cam+0x2C  (far  — irrelevant, see below)
lfs   f3, 0x28(r4)          ; cam+0x28  (near — irrelevant)
lfs   f2, 0x4C(r4)          ; cam+0x4C  = aspect   (STORED camera aspect, 4/3)
lfs   f1, 0x48(r4)          ; cam+0x48  = fovy
bl    0x8034a404            ; C_MTXPerspective(out=r3, fovy, aspect, near, far)
; then rows 2 and 3 of the 4x4 out are OVERWRITTEN with SDA2 constants:
lfs f1,-0x1710(r2); stfs→+0x20,+0x24   ; out[2][0..1] = K0
lfs f0,-0x1700(r2); stfs→+0x28        ; out[2][2]    = K1
stfs f1→+0x2C,+0x30,+0x34,+0x38       ; out[2][3], out[3][0..2] = K0
lfs f0,-0x170C(r2); stfs→+0x3C        ; out[3][3]    = K2
```

Key consequences:
- `C_MTXPerspective` writes near/far only into rows 2/3 — which are then overwritten. So the
  effect matrix is **exactly**:
  ```
  [ cot(fovy/2)/aspect, 0,            0,  0 ]
  [ 0,                  cot(fovy/2),  0,  0 ]
  [ K0, K0, K1, K0 ]
  [ K0, K0, K0, K2 ]
  ```
  (C_MTXPerspective rows 0/1 per the SDK; cot computed with the guest tanf.)
- K0/K1/K2 numeric values UNVERIFIED (SDA2 r2-relative loads; by shape K0=0, K1/K2 = the
  projective row constants). The port never touches rows 2/3, so their values don't matter.
- **The aspect used is the camera's STORED 4:3 aspect (cam+0x4C), not the live GX
  projection.** This is the widescreen water-misalignment root cause: under the anamorphic
  scheme the raster projection m00 is squeezed ×0.75 but this lookup m00 is not, so the
  sampled screen-texture column sits 4/3× too far from centre.

### Other callers of 0x8022ba74 (same class, same fix) **[decomp, caller set from generated code]**

TMapStaticObj::perform (sea/SeaIndirect/pollution seas), TShimmer (removed by design in
this port), telesa (Boo, screen distortion body), namekuri (slug), TIceBlock,
TBEelTears, TNpcParts star glow, MapObjBlock — every screen-texture refraction material in
the game funnels through this one helper. One seam fixes all of them.

## 3. TModelWaterManager — FLUDD/Mario water droplets (the "spray" water)

`reference/sms/src/Player/ModelWaterManager.cpp`; perform 0x8027beb0 dispatches: **[decomp]**

| pass | func | aspect-sensitive? |
|---|---|---|
| move/calc | move/calcWorldMinMax/calcDrawVtx/calcVMAll | no (world/view space) |
| drawSilhouette 0x8027dd00 | view-space metaball spheres | no — live (squeezed) projection applies |
| drawWaterVolume 0x8027d418 | view-space volume + full-screen ±1000 dst-alpha quad | no (quad covers either frustum) |
| drawMirror 0x8027cc2c | mirror-buffer variant | no (mirror pipeline handled in efbtex_widescreen) |
| drawShineShadowVolume 0x8027c67c | spheres + ±1000 alpha quad | no |
| **drawRefracAndSpec 0x8027c12c** | **screen-texture refraction of the droplets** | **YES** |

### drawRefracAndSpec 0x8027c12c **[disasm + decomp, 1:1 match]**

Three blend passes over the same pre-built droplet quadstrip display list (`unk5D30->draw()`,
gated by `unk5D60` bits 2/4/8):

1. **Refraction pass**: identity PNMTX; texgen0 = `GX_TG_MTX3x4` from `GX_TG_POS` via
   TEXMTX0 (0x1E), texgen1 = MTX3x4 from TEX0 via TEXMTX2 (0x3C). TEXMTX0 is built inline:
   ```
   lfs f3, -0x83C(r2)   ; 0.5      (scaleS)   [value by SDK call shape; decomp: 0.5f]
   fmr f5, f3           ; 0.5      (transS)
   lfs f4, -0x838(r2)   ; -0.5     (scaleT)
   fmr f6, f3           ; 0.5      (transT)
   lwz r4, -0x7118(r13) ; gpCamera
   lfs f2, 0x4C(r4)     ; aspect (stored 4:3)
   lfs f1, 0x48(r4)     ; fovy
   bl  0x8034a17c       ; C_MTXLightPerspective(m, fovy, aspect, .5, -.5, .5, .5)
   bl  GXLoadTexMtxImm(m, 0x1E, GX_MTX3x4)
   ```
   (bl target 0x8027c1f0+0xcdf8c = 0x8034a17c verified **[disasm]**; the exact float
   constants ±0.5 are from the decomp — r2-relative loads, values UNVERIFIED but the SDK
   convention and decomp agree.)
   One indirect stage: ind texcoord = texgen1 sampling TEXMAP1 (wave bump), `GXSetTevIndWarp`
   with ind-matrix `[[unk5D1C,0,0],[0,unk5D1C,0]]` (`lfs f0, 0x5D1C(r31)` **[disasm]**) —
   the wave warp magnitude. TEXMAP0 = `unk5D34` = the screen texture; TEV stage0 = TEXC,
   alpha A0 (unk5D65), stage1 modulates alpha by TEXMAP2; src-alpha blend, Z LEQUAL no-update.
2. **Specular/film pass**: indirect off, texgen0 = MTX2x4 from TEX0 (0x3C), TEXMAP2,
   color C0 = `gModelWaterManagerWaterColor[unk5D5F]`, src-alpha blend.
3. **Highlight pass**: TEXMAP3 (`unk5D40`), C0/C1 = unk5D20/unk5D24, additive
   (`GX_BL_SRCALPHA, GX_BL_ONE`).

`C_MTXLightPerspective` 0x8034a17c **[disasm]** is the stock SDK function:
`cot = 1/tan(fovy·0.5·π/180)`; `m[0][0] = (cot/aspect)·scaleS`, `m[0][2] = -transS`,
`m[1][1] = cot·scaleT`, `m[1][2] = -transT`, `m[2][2] = -1`, rest 0 (3x4). Only
`m[0][0]` depends on aspect → same 4:3-stored-aspect mismatch as the sea.

Passes 2/3 use TEX0 coordinates from the display list — aspect-independent. The droplet
GEOMETRY (calcDrawVtx quads) is view-space → covered by the live squeezed projection.

### Other C_MTXLightPerspective callers (must NOT be touched)

- Mirror pipeline (`TMirrorCamera::drawSetting` / mirror manager): the mirror TEXTURE is
  deliberately rendered UNsqueezed (`g_ws_persp_suspend`, efbtex_widescreen.cpp) to match
  its unsqueezed lookup. Scaling its lookup too would re-break it.
- JPA `GenPrjMtx`/`GenPrjTexMtx` particle projection: not screen-texture sampling;
  left alone (revisit only if a projected-texture particle misaligns).
Hence the fix is SCOPED to drawRefracAndSpec, not applied at 0x8034a17c globally.

## 4. Splash / spray quads (no port needed — verified previously)

- `TSplashManager` 0x80266b44/0x80266d64 (Player/SplashManager.cpp): water-splash
  billboards built in VIEW space (identity pos mtx over view-transformed positions,
  perspective projection) — covered by the widened 3D projection. **[decomp + prior
  verification, widescreen_effects.md]**
- `TBathWaterManager` draw_mist EFB replay 0x801aa6cc — already handled
  (`screenfx_widescreen.cpp`).
- `TWaterManager` as a class name does not exist in GMSE01; the spray/splash systems are
  TModelWaterManager (FLUDD droplets) + TSplashManager (surface splashes) +
  EffectColumWater actors (JPA particles, covered by the widened perspective).

## 5. Port summary (water_native.cpp)

Native ownership of the two lookup-matrix derivations, parameterized by an aspect factor:

- `water_lookup_scale()` = 1.0 (default, guest-identical 4:3 math → bit-exact A/B) or
  `ws_squeeze_scale()` ALWAYS (default since 2026-06-12 — the gate is removed; at 4:3 the
scale is 1.0 so guest math is untouched by construction) (true-aspect: lookup m00 matches the
  squeezed raster m00, because raster_m00 = guest_m00 × squeeze ⇒ lookup must scale the same).
- Seam 1: override 0x8022ba74 — guest body via `recomp_raw` (keeps guest trig bit-exact),
  then `out[0][0] ×= scale` computed natively. Multiplication is the EXACT aspect
  parameterization (m00 ∝ 1/aspect; everything else aspect-free per the derivations above).
- Seam 2: scope flag around 0x8027c12c + override on 0x8034a17c applying `m[0][0] ×= scale`
  only inside the scope.
- Pure-math reference implementations (`water_effect_m00`, `water_lightpersp_m00`) live in
  the file as testable functions and document the closed-form values.

## Verification key — RESULTS (2026-06-12)

User-verified live at 16:9: the pier/sea silhouette smear is gone with the scaled lookup
(then-`SUNBRIGHT_WATER_WS=1`, now default). 4:3 is untouched by construction (scale 1.0 →
no memory write). Remaining items below were the original plan; droplet/mirror checks
still worth an eyeball on next gameplay session.

1. **4:3 bit-exactness gate**: `SUNBRIGHT_WIDESCREEN=0` (and WATER_WS unset) — frame-dump
   A/B vs a build without water_native.cpp must be pixel-identical (scale=1.0 path writes
   back the unmodified value only in WS mode; in default mode it does not touch memory).
2. **Sea path**: Delfino Plaza from spawn looking at the sea (also exercises
   SeaIndirect + the effect-mtx seam). Expect: refraction image lines up with the scene
   behind the water at screen edges under `SUNBRIGHT_WATER_WS=1` + widescreen.
3. **Droplet path**: spray FLUDD toward the camera / dive into water (drawRefracAndSpec,
   needs `unk5D60 & 2` active). Watch droplet refraction at frame edges.
4. **Hose splash / fountains**: TSplashManager + EffectColumWater — should be unchanged in
   all modes (no override fires).
5. **Mirror regression check**: Hotel Delfino bathroom mirror (or Sirena) — must be
   unchanged (scoped fix must not reach the mirror's C_MTXLightPerspective).
