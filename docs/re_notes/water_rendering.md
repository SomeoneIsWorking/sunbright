# Water rendering RE — Delfino Plaza water (for 60fps re-issue fidelity)

Goal: understand exactly how SMS renders the plaza water so the in-between (interpolated)
field can render identically to the real field. The user observes (a) the water surface
**flickering** (an effect present on one field, gone/different on the next) and (b) a
**stale screen-texture reflection ghost** of moving objects.

All addresses are GMSE01 (`reference/sms_gmse01_funcs.txt`). All file:line refer to the
vendored decomp at `reference/sms/`.

---

## 0. TL;DR conclusion (read this first)

The plaza water reflection is a **planar refraction that samples a half-res copy of the
EFB** (the "screen texture", スクリーンテクスチャ). The flow is, per real frame:

1. The whole opaque/translucent scene is drawn into the EFB.
2. A `TEfbCtrlTex` ("通常シーン描画ステージ") **copies the EFB into the screen-texture
   JUTTexture** via `GXCopyTex` (`TEfbCtrlTex::perform` `0x802f8bac`, copy branch when
   `param_1 & 0x8`).
3. Later, in a *post* phase (`param_1 & 0x80`), `TModelWaterManager::drawRefracAndSpec`
   (`0x802c12c`… i.e. `0x8027c12c`) binds that screen texture as `GX_TEXMAP0` and draws the
   water quads, using an **indirect texture warp** (the `waterref` bump map) to perturb the
   screen-space lookup, plus a separate specular/sparkle pass.

So the water's pixels are literally **a warped re-projection of whatever was in the EFB a
moment earlier**. That is why:

- **Re-issuing the water draw against a different EFB changes the result.** The water reads
  the EFB *as a texture*. On the in-between field we re-run the world draw, so the EFB (and
  the screen-texture copy taken from it) holds the interpolated world — the reflection then
  reflects the interpolated scene. If we instead skip the copy and re-use the previous
  copy, the reflection reflects the *previous* world while the surface geometry/refraction is
  at the new position → mismatch = flicker.
- **Re-using a STALE screen texture ghosts moving objects.** The screen texture is a frozen
  snapshot. If a moving object (NPC, Mario, a boat) moved between the real field and the
  in-between field, the reflection still shows it at the old position → a ghost/double-image
  in the water.

The surface **sparkle/glint animation** (`waterSpec` pass and the shimmer SRT scroll) is
driven by **discrete frame-stepped controllers**, not by `mftb`/wall-clock — so two fields
drawn microseconds apart will use the **same** phase *iff* the controller is not advanced
twice. The controllers are advanced only in the movement/calc phases (`param_1 & 1`), so a
draw-only re-issue is phase-stable; but if the re-issue accidentally re-runs the movement
phase it will double-step and the surface texture jumps.

---

## 1. The water objects and what each draws

### TModelWaterManager — the player/spray water *and the plaza water surface refraction*
`reference/sms/src/Player/ModelWaterManager.cpp`. vtable `perform` = `0x8027beb0`.

This is the central water object. Despite the name it owns both Mario's sprayed-water
particles **and** the screen-space water-surface refraction/specular pass.

`TModelWaterManager::perform(u32 param_1, TGraphics*)` (ModelWaterManager.cpp:1541,
`0x8027beb0`) is phase-multiplexed:

- `param_1 & 1`  → `move()` + `calcWorldMinMax()`; `unk5E00 += 1` (its own frame counter).
- `param_1 & 4`  → `calcDrawVtx` + `calcVMAll` (build particle matrices from the view mtx).
- `param_1 & 8`  → `drawSilhouette`, `drawWaterVolume`, `drawMirror`, `drawShineShadowVolume`
  (line 1562–1578). This is the in-EFB part: stencils into dst-alpha, draws the water
  volume, the round mirror under Mario, etc.
- `param_1 & 0x80` → **`drawRefracAndSpec()`** (line 1580–1591) — the screen-texture
  refraction + specular sparkle. This is the *post* pass that runs after the EFB copy.

Sub-draws of interest:
- `drawRefracAndSpec` (ModelWaterManager.cpp:1442, `0x8027c12c`) — **the reflection/refraction**.
  Detailed in §2.
- `drawWaterVolume` (1442 above is refrac; volume is at :997, `0x8027d418`) — writes the
  water body into dst-alpha and color using `gModelWaterManagerWaterColor[unk5D5F]`.
- `drawMirror` (:1161, `0x8027cc2c`) — the soft round reflection disc centered on Mario
  (`SMS_GetMarioPos()`), 4-point fan; **Mario-position-dependent**.
- `drawSilhouette` (:920, `0x8027dd00`) and `drawTouching` (:889, `0x8027e0ec`).

Screen-texture handle is cached in `loadAfter` (`0x80280088`):
```
unk5D34 = TNameRefGen::search<TScreenTexture>("スクリーンテクスチャ")->getTexture();  // :194
```
`unk5D34` is the `JUTTexture*` bound as `GX_TEXMAP0` in `drawRefracAndSpec` (:1474).

### TScreenTexture — the EFB-copy target ("スクリーンテクスチャ")
`reference/sms/src/MarioUtil/ScreenUtil.cpp`. `load` `0x8022d474`, `replace` `0x8022d360`.

`TScreenTexture::load` (:215) allocates a **half-resolution RGB565** JUTTexture:
```
unk10 = new JUTTexture(SMSGetGameRenderWidth()/2, SMSGetGameRenderHeight()/2, GX_TF_RGB565);  // :218
gpScreenTexture = this;
```
This is the texture every water/shimmer/underwater object samples. `replace()` swaps a
model's named texture's `ResTIMG` to point at this texture (used by underwater/shimmer to
inject the live screen texture into their .bmd materials).

### TAfterEffect — full-screen dash blur (also samples the screen texture)
`ScreenUtil.cpp`, `perform` `0x8022d4f8`. Binds the same screen texture (`unk10`, set in
`loadAfter` :42 from "スクリーンテクスチャ") and draws a blurred full-screen quad when
`param_1 & 0x10`. Not the plaza water, but it is a second consumer of the same EFB copy;
its texgen is `GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, 0x3c, …)` (:144),
identity-ish screen-space mapping.

### TShimmer — the heat-haze / water-shimmer mapobj
`reference/sms/src/Map/Shimmer.cpp`. `perform` `0x8019f83c`, `loadAfter` `0x8019f740`.

`loadAfter` (:84) injects the screen texture into the shimmer model's material slot 1:
```
unk44->getTexture()->setResTIMG(1, *TNameRefGen::search<TScreenTexture>("スクリーンテクスチャ")->getTexture()->getTexInfo());  // :89
```
`perform` (:28):
- `param_1 & 1`  → `unk54->setFrame(unk58->getFrame()); unk58->update();` — **advances the
  BTK (texture SRT) animation by one frame.** This is the shimmer's scroll/wobble phase.
  `unk58` is a `J3DFrameCtrl` (looping, `init(unk54->getFrameMax())`, Shimmer.cpp:116).
- `param_1 & 4`  → builds the model matrix from `param_2->mViewMtx` (inverse-view × TR ×
  scale, :70–73) and sets `setEffectMtx(SMS_GetLightPerspectiveForEffectMtx(...))` on texmtx
  1 (:48–55). Mario-dependent placement (mPosition flips to `(0,0,9600)` vs `(0,0,0)` based
  on cap/ground/pool flags, :39–46).
- `param_1 & 0x200` → `unk48->update()` (J3DModel update).

### TMapObjSeaIndirect / TMapObjWaterFilter — the deep-water "under water" layer
`reference/sms/src/MoveBG/MapObjWater.cpp`. `TMapObjWaterFilter::perform` `0x801ea840`,
`init` `0x801ea7f4`.

`TMapObjSeaIndirect::init` (:28) loads `/common/map/UNDERwater.bmd` + `underwater` BTK and
**injects the screen texture into material texture slot 1** (:35–38) — same screen-texture
mechanism. `TMapObjWaterFilter::perform` (:47) is the underwater overlay shown when the
camera is below the water height (`gpCamera->unk124.y < waterHeight`, :63); it builds an
inverse-view matrix and calls `unk44->perform`.

---

## 2. The refraction/reflection mechanism (the crux)

### 2a. Who captures the EFB into the screen texture, and when

`JDrama::TEfbCtrlTex` is the EFB→texture copier. Its `perform` (`0x802f8bac`,
JDREfbCtrl.cpp:80) does, in the **copy phase** (`param_1 & 0x8`):
```
GXSetTexCopySrc(unk10.x1, unk10.y1, unk10.getWidth(), unk10.getHeight());     // :96
GXSetTexCopyDst(mWidth, mHeight, mTexFmt, unk20.check(0x1000));               // :98  (half-res, mipmap?)
GXCopyTex(mImagePtr, doClear);                                                // :99  ← EFB → screen-texture image
```
`mImagePtr` is the screen texture's image buffer, wired up in `setTexAttb`
(`0x802f8b40`, :65) which `GXGetTexObjAll`s the texture object passed in.

The plaza's instance is **"通常シーン描画ステージ"** ("normal-scene draw stage"), set up in
`TMarDirector::setupObjects` (MarDirectorSetupObjects.cpp:387–400):
```
normalSceneDrawStage = TNameRefGen::search<TEfbCtrlTex>("通常シーン描画ステージ");  // :387
normalSceneDrawStage->unk20.on(0x122F);                  // :390  enables format+copy bits incl. 0x8 copy
normalSceneDrawStage->unk44 = SMSVFilter_flicker;        // :391  copy filter (anti-flicker vertical filter)
GXTexObj sctex = screenTex->getTexture()->mTexObj;       // :395  the スクリーンテクスチャ JUTTexture
normalSceneDrawStage->setTexAttb(sctex);                 // :396  → mImagePtr = screen texture buffer
normalSceneDrawStage->setSrcRect(TRect(0,0,renderW,renderH));  // :398-400  copy whole frame
```
So the copy source is the **full render rect** and the dst is the **half-res RGB565** screen
texture. The copy is issued during the GX perform list at the link's copy phase (`& 0x8`).

### 2b. Per-frame phase ordering (why copy-then-water matters)

Phases are broadcast by `TMarDirector::direct()` (MarDirectorDirect.cpp:166–177), once per
displayed frame:
```
unk40->perform(0xffffffff, …); unk38->… ; unk3C->… ;       // :168-170
mPerformListGX->perform(0xffffffff, &local_140);           // :171  main scene GX list
… mPerformListSilhouette->perform(0xffffffff, …);          // :174
mPerformListGXPost->perform(0xffffffff, &local_140);       // :176  POST list
```
`TPerformList::perform` → `forEachPerform` calls `link->unk4->testPerform(param_4 & link->unk8, …)`
(PerformList.cpp:11). i.e. each viewobj receives `0xffffffff & mask`, and its `perform`
internally branches on the phase bits (`& 1 / & 4 / & 8 / & 0x80`). The per-object phase
masks come from data (`/data/PerformLists.bin` + scene.bin), not source.

Ordering that matters for fidelity:
1. **mPerformListGX** draws the world into the EFB. The **EFB→screen-texture copy** (the
   "通常シーン描画ステージ" `TEfbCtrlTex`) fires here, in its copy phase, **after** the
   world is in the EFB. (Mask 0x122F includes the 0x8 copy bit.)
2. **mPerformListGXPost** then runs the water's `0x80` phase →
   `drawRefracAndSpec()` samples the just-captured screen texture.

So within one real frame the screen texture is a snapshot of *this frame's* opaque/translucent
world, and the water reflects it. Re-running mPerformListGX on the in-between field re-copies
the EFB; skipping it leaves last field's snapshot.

### 2c. How the water material samples the screen texture (drawRefracAndSpec)

`TModelWaterManager::drawRefracAndSpec` (ModelWaterManager.cpp:1442, `0x8027c12c`):

Tex bindings (lines 1474–1476):
```
unk5D34->load(GX_TEXMAP0);  // スクリーンテクスチャ (the EFB copy)
unk5D38->load(GX_TEXMAP1);  // waterref.bti  (the bump/normal map for indirect warp)
unk5D3C->load(GX_TEXMAP2);  // waterMask.bti (mask)
… later unk5D40->load(GX_TEXMAP3);  // waterSpec.bti (the sun-glint sparkle, :1523)
```

Tex-coord generation (lines 1453–1459) — **this is the screen-space projection**:
```
GXSetNumTexGens(2);
GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_POS, 0x1e, 0, 0x7d);  // POS → texmtx 0x1e
GXSetTexCoordGen2(GX_TEXCOORD1, GX_TG_MTX3x4, GX_TG_TEX0, 0x3c, 0, 0x7d);
Mtx m;
C_MTXLightPerspective(m, gpCamera->getFovy(), gpCamera->getAspect(),
                      0.5f, -0.5f, 0.5f, 0.5f);   // :1457  builds the screen-projection texmtx
GXLoadTexMtxImm(m, 0x1e, GX_MTX3x4);              // :1459
```
`C_MTXLightPerspective` (`0x8034a17c`) builds a perspective→[0,1] texture-projection matrix
from the **camera's current fovy and aspect** (`gpCamera->getFovy()/getAspect()`). TEXCOORD0
= the world position projected through this camera-derived matrix → it maps each water vertex
to **its screen-space UV in the screen texture**. That is the planar reflection projection:
the water samples the EFB copy at (roughly) the pixel under that water vertex.

Indirect warp (the *refraction* wobble), lines 1460–1471:
```
GXSetNumIndStages(1);
GXSetIndTexOrder(GX_INDTEXSTAGE0, GX_TEXCOORD1, GX_TEXMAP1);   // waterref drives the warp
GXSetTevIndWarp(GX_TEVSTAGE0, GX_INDTEXSTAGE0, 1, 0, GX_ITM_0);
local_84 = {{unk5D1C,0,0},{0,unk5D1C,0}};                       // warp scale = unk5D1C (0.07, set :112)
GXSetIndTexMtx(GX_ITM_0, local_84, 0);
```
Stage 0 then samples `GX_TEXMAP0` (the screen texture) at TEXCOORD0 **as displaced by the
indirect `waterref` lookup** → the rippled reflection. TEV stages combine screen-tex color →
masked by waterMask (stage1, :1487) → tinted by `gModelWaterManagerWaterColor[unk5D5F]`.

`unk5D30->draw()` (the `TDLTexQuad` water quad buffer, created `createBuffer(256)` at :119)
is what's actually emitted, gated by surface flags `unk5D60 & 2 / & 4 / & 8` (:1503, :1521,
:1537) for the three sub-passes (refraction, water-color, specular sparkle).

### 2d. Why re-issuing against a different EFB changes the result; why a stale texture ghosts

- The reflection color **is** the screen texture, sampled per the camera-projection texmtx
  (§2c). The screen texture is the EFB contents at copy time (§2a). Therefore the water
  output is a pure function of (EFB copy, camera fovy/aspect, water vertex positions, indirect
  warp phase). **Change the EFB → change every reflected pixel.** On the in-between field, if
  we re-render the world we get a *new* EFB (interpolated world); if we re-copy it the
  reflection is self-consistent with the interpolated surface. If we **don't** re-copy and
  reuse the previous copy, the surface geometry/refraction warp is at the new state but the
  reflected content is one field old → the surface "shimmers"/flickers (different reflected
  content on alternating fields). This is almost certainly the reported water flicker.
- **Ghosting**: a moving object that is in the EFB at copy time is baked into the screen
  texture at its position *at that instant*. If the in-between field reuses a stale copy, the
  reflection shows the object at its previous-field position, offset from where the object now
  is in the directly-rendered scene → a ghost/double of moving objects in the water.

---

## 3. Surface animation phase source (frame counter vs time base)

Two animated surface elements; **both are discrete frame-stepped, not wall-clock**:

1. **Shimmer SRT scroll** (`TShimmer`): a `J3DFrameCtrl` (`unk58`) advanced exactly once per
   movement phase: `unk54->setFrame(unk58->getFrame()); unk58->update();` only inside
   `if (param_1 & 1)` (Shimmer.cpp:33–36). `J3DFrameCtrl::update` is an integer/looping
   frame stepper (one tick per call). **Phase source = number of `& 1` perform calls**, i.e.
   the logical game-frame count — NOT `mftb`/`OSGetTime`.

2. **Sun-glint sparkle** (`waterSpec`, TEXMAP3 in `drawRefracAndSpec`, :1523): the sparkle's
   on-screen motion comes from the **camera-projection texmtx** (`C_MTXLightPerspective` of
   the live camera fovy/aspect, :1457) modulated by the water-color registers `unk5D20`
   /`unk5D24` (constants set in `load`, :113–114). There is **no per-frame phase counter for
   the spec pass itself** — it moves only because the camera moves and because the underlying
   reflection (screen tex) and indirect warp change. The indirect-warp `waterref`/`waterMask`
   textures are static `.bti`s; the warp scale `unk5D1C` is a constant (0.07). So the
   specular highlight position is a **pure function of camera + screen texture**, with no
   independent time/frame phase.

3. `TModelWaterManager`'s own counter `unk5E00` (incremented at perform.cpp:1548 under
   `param_1 & 1`) feeds `askDoWaterHitCheck()` (`unk5E00 % unk5E04`, :372) — gameplay hit
   throttling, **not** a visual phase. Irrelevant to the surface look but **must not be
   double-incremented** by a re-issue.

**No `mftb`/`OSGetTime`/`OSGetTick` appears anywhere in the water/shimmer draw paths.** All
animation phase is the integer perform-`& 1` (movement) frame count. Consequence: two fields
drawn microseconds apart land on the **same** surface phase as long as the `& 1` phase is run
only once per real game-step. A draw-only re-issue (running only `& 4`/`& 8`/`& 0x80`) is
phase-stable. A re-issue that re-runs `& 1` would double-step the shimmer SRT and the hit
counter → visible texture jump.

---

## 4. Which perform phase the water draws in

Per-frame the director broadcasts (MarDirectorDirect.cpp:166–177):
- `mPerformListGX->perform(0xffffffff)` (:171) — main scene into EFB; the
  **"通常シーン描画ステージ" `TEfbCtrlTex` EFB→screen-texture copy** fires here (copy bit
  `& 0x8`, mask 0x122F). The water's in-EFB sub-passes (silhouette/volume/mirror/shine,
  `& 0x8`) also run in this stage.
- `mPerformListGXPost->perform(0xffffffff)` (:176) — **`TModelWaterManager::perform` runs its
  `& 0x80` branch → `drawRefracAndSpec()`** here, after the copy. (`TAfterEffect` dash-blur,
  `& 0x10`, is in this post region too.)

So: **screen-texture capture = GX list, copy phase 0x8; water refraction = GX-Post list,
phase 0x80.** Movement/animation (`& 1`) and matrix calc (`& 4`) run earlier in the frame via
`mPerformListMovement`/`mPerformListCalcAnim` (MarDirectorSetupObjects.cpp:425–475,
MarDirectorDirect.cpp:152–158). Cross-ref `MarDirectorDirect.cpp` (`direct()`), not a
single `direct()` address recorded here (the function spans the file).

---

## 5. CONCLUSION — per-field-varying inputs to the water render

For each input the water reflection/surface depends on, classify for 60fps re-issue:

| Input | Source | Classification for in-between field |
|---|---|---|
| **Screen texture (EFB copy)** | `GXCopyTex` of the EFB by "通常シーン描画ステージ" `TEfbCtrlTex` (`0x802f8bac`), copy phase `& 0x8` | **Must be re-captured from the in-between EFB.** If you re-render the world for the in-between field, you MUST also re-run the EFB→screen-texture copy so the reflection matches the surface you're drawing. Re-using the real field's copy = the reported flicker + moving-object ghost. If you do NOT re-render the world for the in-between field, do NOT re-draw the water either (it would reflect a frozen world over a moved surface). |
| **Camera fovy/aspect (projection texmtx)** | `C_MTXLightPerspective(gpCamera->getFovy(), getAspect(), …)` (ModelWaterManager.cpp:1457) | **Safe to re-issue** — read live each draw; if the camera is interpolated for the in-between field, the projection follows correctly. Just read `gpCamera` once consistently. |
| **View matrix (`param_2->mViewMtx`)** | passed into `perform` (:1543), used by mirror/volume/shimmer | **Safe to re-issue** if the same (interpolated) view mtx is threaded through. Must be the *same* view used for the world draw of that field (don't mix fields). |
| **Mario position (`SMS_GetMarioPos()`)** — round mirror disc (`drawMirror` :1240) and the shimmer Z-flip (`mPosition` Shimmer.cpp:43) | live global | **Safe to re-issue, BUT must use the field-consistent value.** If Mario is interpolated for the in-between field, read the interpolated position; if not interpolated, the disc will pop. Tie it to whatever Mario transform the rest of that field used. |
| **Shimmer SRT scroll phase** (`J3DFrameCtrl unk58`, advanced under `& 1`) | integer frame counter (Shimmer.cpp:33-36) | **Frame-counter based → will desync if `& 1` runs twice.** Advance it ONCE per real game step. The in-between re-issue must run only draw phases (`& 4/& 8/& 0x80`), never `& 1`. With that, the shimmer phase is identical on both fields (correct). |
| **`TModelWaterManager::unk5E00`** (perform.cpp:1548, under `& 1`) | integer counter (hit-check throttle, not visual) | **Frame-counter based; do not double-increment.** Same rule as above — keep `& 1` single-stepped. Visually irrelevant but desyncs gameplay hit checks if doubled. |
| **Sun-glint sparkle (`waterSpec`)** | pure function of camera + screen texture + static `.bti` (no independent phase) | **Safe to re-issue** — has no time/frame phase of its own; it follows the camera and screen texture. It will look right on the in-between field iff the screen texture and camera are field-consistent (see rows 1–2). |
| **Indirect warp (`waterref`/`waterMask`, scale `unk5D1C`)** | static textures + constant (ModelWaterManager.cpp:1465-1471) | **Safe to re-issue** — fully static; identical on both fields by construction. |
| **Surface flags / water color (`unk5D60`, `unk5D5F`, `gModelWaterManagerWaterColor`)** | state set in load/loadAfter, toggled by gameplay | **Safe to re-issue** — not per-field; constant within a frame. |

### Net guidance for the renderer
- The water reflection is **EFB-derived**. The single most important rule: **the screen
  texture and the water surface draw must come from the same field.** Either re-render the
  world AND re-copy the EFB AND re-draw the water for the in-between field (all three
  together), or do none of them. Mixing a stale screen texture with a freshly-positioned
  water surface is exactly the flicker + ghost the user sees.
- All surface *animation* is integer-frame-stepped (`J3DFrameCtrl`, `unk5E00`), advanced only
  in the movement phase (`& 1`). The in-between re-issue must execute **draw phases only**
  (`& 4`, `& 8`, `& 0x80`) and never the movement phase, so the surface texture phase stays
  identical between the two fields. There is **no wall-clock/`mftb` phase** to worry about.
- Camera, view matrix, and Mario position are read live and are safe **provided the
  interpolated values used for the water match those used for the world draw of that same
  field**.

### Open / uncertain
- The exact per-object phase masks for `TModelWaterManager`, the shimmer, and the
  "通常シーン描画ステージ" `TEfbCtrlTex` are **data-driven** (`/data/PerformLists.bin`,
  scene.bin) and not in the decomp source — verify the live masks at runtime (probe the
  perform-list link `unk8` values) if exact bit gating matters.
- `SMS_GetLightPerspectiveForEffectMtx` (used by `TShimmer`, MapObjBlock, etc.) is declared
  (MtxUtil.hpp:160) but **its definition is not in the vendored decomp** — its contents
  (whether it folds in a time/light phase) are unverified here. The plaza water surface
  (`drawRefracAndSpec`) does NOT use it; it uses `C_MTXLightPerspective` directly with live
  camera params, so the plaza-water analysis above is unaffected. Flagged for follow-up only
  if the *shimmer* layer (not the main water) is the flickering element.
