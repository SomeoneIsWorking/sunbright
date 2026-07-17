# Water refraction projection — WHICH camera/matrix the screen-texture texgen reads

Pins exactly which camera/projection source the plaza-water refraction texgen uses, so the
60fps in-between can freeze **all** of it to tick N consistently. This is the follow-up to
the failed fix that froze only `j3dSys.mViewMtx` (0x804045DC) + skipped the EFB copy and
**still ghosted Mario into the sky**.

Addresses are GMSE01 (`reference/sms_gmse01_funcs.txt`); file:line refer to `decomp/sms/`.

---

## 0. TL;DR — the failed fix froze the WRONG matrix

`TModelWaterManager::drawRefracAndSpec` (ModelWaterManager.cpp:1442, `0x8027c12c`) does **not**
read `j3dSys.mViewMtx` (0x804045DC) at all. Its screen-space refraction is the product of TWO
camera inputs, and the failed fix froze neither of the two:

1. **The screen-projection texmtx (texmtx slot 0x1e)** is built from
   **`gpCamera->getFovy()` and `gpCamera->getAspect()`** — the **live `gpCamera` globals**
   `mFovy`@0x48 / `mAspect`@0x4C — via `C_MTXLightPerspective` (line 1457). It is a pure
   eye-space→[0,1] projection (fovy+aspect only, NO view rotation/translation; see §2).
2. **The water quad geometry it projects (`unk5D30`)** is built in **eye/view space** from
   **`param_2->mViewMtx` = `gfx + 0xB4`** in the `& 4` calc phase (`calcVMAll`, line 1556;
   `calcVMMtxGround/Wall` pre-multiply by that view matrix, §3). The refraction draw itself
   loads **identity** into PNMTX0 (line 1448), so the quad POS must already be eye-space.

The texgen (`GX_TG_POS` → texmtx 0x1e, line 1454) therefore maps **eye-space-quad-vertex →
screen UV** purely with the projection from `gpCamera`. The view rotation/translation enters
**only** through the eye-space quad vertices (built from `gfx+0xB4`).

So the projection of the reused (tick-N) screen texture is governed by **`gfx+0xB4`** (quad
eye-space transform) **and `gpCamera->mFovy/mAspect`** (the texmtx) — NOT by `j3dSys.mViewMtx`.
`interp_redraw.cpp` writes the **blended** view into BOTH `j3dSys` AND `gfx+0xB4`
(interp_redraw.cpp:246-248). Restoring only `j3dSys` left `gfx+0xB4` blended → the water quad
was built with the **interpolated** camera while the reflected content is the **tick-N**
texture → the ghost. **Point 3 of the task: CONFIRMED — the water reads `gfx+0xB4`, that is why
restoring only `j3dSys` failed.**

---

## 1. The texgen / texmtx setup (drawRefracAndSpec, line 1442)

Exact reads, with the camera/projection input flagged:

```c
PSMTXIdentity(afStack_3c);                                   // :1446  identity POS matrix
GXSetCurrentMtx(GX_PNMTX0);
GXLoadPosMtxImm(afStack_3c, GX_PNMTX0);                      // :1448  PNMTX0 = IDENTITY  ← quad POS is used raw
GXLoadNrmMtxImm(afStack_3c, GX_PNMTX0);                      // :1449
GXSetNumTexGens(2);
GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_POS,  0x1e, 0, 0x7d); // :1454  TEXCOORD0 = POS · texmtx0x1e
GXSetTexCoordGen2(GX_TEXCOORD1, GX_TG_MTX3x4, GX_TG_TEX0, 0x3c, 0, 0x7d); // :1455  TEXCOORD1 = TEX0 · identity(0x3c)
Mtx afStack_6c;
C_MTXLightPerspective(afStack_6c, gpCamera->getFovy(),      // :1457  ← LIVE gpCamera->mFovy  (0x48)
                      gpCamera->getAspect(),               // :1458  ← LIVE gpCamera->mAspect (0x4C)
                      0.5f, -0.5f, 0.5f, 0.5f);
GXLoadTexMtxImm(afStack_6c, 0x1e, GX_MTX3x4);               // :1459  → texmtx slot 0x1e (the one TEXCOORD0 uses)
...
unk5D34->load(GX_TEXMAP0);   // :1474  スクリーンテクスチャ (the EFB copy)  — the refracted content
unk5D38->load(GX_TEXMAP1);   // :1475  waterref.bti  (indirect warp, static)
unk5D3C->load(GX_TEXMAP2);   // :1476  waterMask.bti (static)
...
unk5D40->load(GX_TEXMAP3);   // :1523  waterSpec.bti (static)
if (unk5D60 & 2) unk5D30->draw();  // :1504  refraction pass (the dominant layer)
```

`getFovy()`/`getAspect()` are plain field reads (JDRCamera.hpp:100-101:
`f32 getFovy() const { return mFovy; }`, `getAspect()` → `mAspect`). `gpCamera` is the live
global gameplay camera `CPolarSubCamera*` (cameragc.cpp:35; see `camera_view_matrix.md` §1.2).

There is **no read of `j3dSys` (0x804045DC), no read of `gfx->mProjMtx` (+0x74), and no read
of `gfx->mViewMtx` (+0xB4) inside `drawRefracAndSpec` itself.** The only camera state it reads
directly is the two scalars `gpCamera->mFovy` / `mAspect`.

---

## 2. C_MTXLightPerspective depends ONLY on fovy/aspect (no view)

`C_MTXLightPerspective` (mtx.c:524):
```c
cot     = 1 / tan(0.5*fovY * deg2rad);
m[0][0] = scaleS * (cot / aspect);  m[0][2] = -transS;     // = 0.5*(cot/aspect), -0.5
m[1][1] = cot * scaleT;             m[1][2] = -transT;     // = -0.5*cot,         -0.5
m[2][2] = -1;
/* every other element 0 */
```
It is a pure perspective-divide → [0,1] texture-projection matrix parameterized **only** by
`fovY`, `aspect`, and the constant `scaleS/T, transS/T` (0.5,-0.5,0.5,0.5 here). It carries
**no camera rotation and no camera translation**. The full camera orientation/position enters
the screen UV exclusively through the **eye-space coordinates** of the quad vertices it
multiplies — i.e. through whatever view matrix built the quad (§3). This is why freezing the
texmtx (fovy/aspect) alone is necessary but not sufficient: the eye-space quad must match.

(For completeness, the other `C_MTXLightPerspective` sites — MapMirror.cpp:40
`unk80 * gpCamera->mFovy`, and JPADrawVisitor.cpp:62/74 `dc->mBaseEmitter->getFovy()` — are the
mirror and JParticle screen-projections; they follow the same fovy/aspect-only rule. Only
ModelWaterManager.cpp:1457 is the water surface.)

---

## 3. The quad geometry (`unk5D30`) is eye-space, built from gfx+0xB4

`unk5D30` is a `TDLTexQuad` (created `createBuffer(256)`, line 119). Its draw (`unk5D30->draw()`,
DLUtil.cpp:97) emits `GX_VA_POS` vertices straight from its POS array — there is **no per-draw
matrix transform** (PNMTX0 is identity in `drawRefracAndSpec`). The POS array is filled in the
`& 4` calc phase:

```c
// TModelWaterManager::perform (:1551)
if (param_1 & 4) {
    calcDrawVtx(param_2->mViewMtx);     // :1555   (empty in this build, :763)
    calcVMAll  (param_2->mViewMtx);     // :1556   ← param_2->mViewMtx == gfx + 0xB4
}
```
`calcVMAll(view)` → `calcVMMtxGround/Wall(view, ...)` (lines 872, 884) which **pre-multiply by
`param_1 = view`** to produce eye-space matrices `unk2D14[i]` (calcVMMtxGround, lines 783-814:
each output row = view-row · particle-local). Those eye-space matrices are loaded as PNMTX0 for
the touching/silhouette/volume sub-draws (`GXLoadPosMtxImm(unk2D14[i], GX_PNMTX0)`,
drawTouching :894/:902, drawSilhouette :959/:967). The refraction quad `unk5D30` is fed the same
eye-space vertices through the request/draw buffer.

**Net:** the quad the refraction texgen projects lives in eye space, and that eye space is
defined by **`gfx->mViewMtx` (+0xB4)** at the moment the `& 4` calc phase ran. (`perform`
caches `MtxPtr r29 = param_2->mViewMtx` at line 1543 and threads it through the `& 8` draws too;
`drawWaterVolume`/`drawMirror`/`drawShineShadowVolume` all read `param_2->mViewMtx`.)

---

## 4. The deep-water / underwater layer also reads gfx+0xB4 and gpCamera

For the plaza specifically, the larger water body is the J3D `UNDERwater.bmd` layer
(`TMapObjSeaIndirect`/`TMapObjWaterFilter`, MapObjWater.cpp), which injects the same screen
texture into a material slot (line 38) and:
- builds its base TR matrix from `MTXInverse(param_2->mViewMtx, ...)` — **gfx+0xB4** (line 84),
- gates on `gpCamera->unk124` (eye position, lines 63-65) and `gpCamera->isSimpleDemoCamera()`
  / `gpCamera->mMode` (lines 55-56).

Its texgen (the screen-texture material's effect matrix) is the .bmd material's
`SMS_GetLightPerspectiveForEffectMtx(...)` (per `water_rendering.md` §1, TShimmer.cpp:48-55) —
camera-derived (definition not in the vendored decomp; flagged uncertain — but it is fed
`param_2`/camera and is not `j3dSys`). So it too is `gfx+0xB4` + `gpCamera`-driven, not
`j3dSys`-driven.

---

## 5. COMPLETE per-field camera/projection state the water reads (where each lives)

| # | State | Read at (file:line) | Where it lives at draw time | Written by interp_redraw? |
|---|---|---|---|---|
| 1 | **Screen texture** (EFB copy) | `unk5D34->load(TEXMAP0)` :1474 | the スクリーンテクスチャ JUTTexture image; filled by `TEfbCtrlTex` `GXCopyTex` (`0x802f8bac`, `& 0x8`) | skipped on in-between via `ov_efbctrltex_perform` (interp_redraw.cpp:112) — reuses tick-N |
| 2 | **Projection texmtx (fovy/aspect)** | `gpCamera->getFovy()/getAspect()` :1457-1458 | `gpCamera->mFovy` (+0x48), `mAspect` (+0x4C) — live global | **NO — never touched** (the gap) |
| 3 | **Quad eye-space verts** | `calcVMAll(param_2->mViewMtx)` :1556 (cached `r29` :1543) | `gfx + 0xB4` (`mViewMtx`) at the `& 4` / `& 8` phase | **YES — blended** (interp_redraw.cpp:248) |
| 4 | View matrix for volume/mirror/shineshadow | `param_2->mViewMtx` :1567,1570,1574,1587 | `gfx + 0xB4` | YES — blended (same as #3) |
| 5 | `j3dSys.mViewMtx` (0x804045DC) | **NOT read by the water** (used by generic J3D models, not drawRefracAndSpec) | J3DSys+0x0 | YES — blended then restored before 0x24 (interp_redraw.cpp:318-320) |
| 6 | indirect warp / mask / spec | static `.bti` + const `unk5D1C`=0.07 | constant | n/a |

The water surface's per-field-varying camera inputs are exactly **#2 (`gpCamera` fovy/aspect)**
and **#3/#4 (`gfx+0xB4` view)**. `j3dSys` (#5) is a red herring for the water.

---

## 6. CONCLUSION — exact freeze list for a consistent (no-ghost, no-flicker) in-between

The refraction projects the **reused tick-N screen texture**. For texture and projection to
agree (no ghost), the projection must also be the **tick-N camera**. The projection = eye-space
quad (`gfx+0xB4`) × `C_MTXLightPerspective(gpCamera->mFovy, mAspect)`. So, in addition to
skipping the EFB copy, **freeze ALL of the following to tick N for the duration of the water /
GXPost (0x24) pass:**

1. **`gfx + 0xB4` (mViewMtx)** — restore to the un-blended tick-N view before the water's `& 4`
   calc AND `& 0x80` draw run. The current code blends this (interp_redraw.cpp:248) and the
   failed fix did **not** restore it (it restored only `j3dSys` at :320). **This is the primary
   missed item — the ghost's direct cause.** Note: the `& 4` calc that builds the eye-space quad
   may run in an earlier list, so the freeze must cover whichever list rebuilds `unk5D30`/
   `unk2D14`, not just the 0x80 post pass — verify at runtime which list issues the water `& 4`.
2. **`gpCamera->mFovy` (+0x48) and `gpCamera->mAspect` (+0x4C)** — must be the tick-N values
   while `drawRefracAndSpec` runs. In normal gameplay these are constant frame-to-frame (aspect
   is config; fovy only animates on zoom/demo), so this is usually a no-op — **but during a fovy
   zoom it is a second per-field input and must be frozen too**, else the texmtx projects the
   tick-N texture through an interpolated fovy. (interp_redraw never interpolates fovy today, so
   today the risk is only if a future change blends projection.)
3. **`j3dSys.mViewMtx` (0x804045DC)** — already restored before 0x24 (interp_redraw.cpp:318-320);
   keep that, but it is **not** what the water reads — its real purpose is any generic J3D model
   in the post list, not the refraction.
4. **`gpMarioPos`** — already blended+restored (interp_redraw.cpp:263-286, :340). The water
   silhouette/marukage shadow ride on `gpMarioPos`; tie its field choice to the water's. For the
   "freeze water at 30fps" path, freeze `gpMarioPos` to tick N for the water+silhouette draw too
   (consistency with the frozen surface), per `water_shadow_determinism.md` §5.

**Is a fully-consistent 60fps freeze possible?** For the WATER LAYER alone, yes: freeze #1+#2 to
tick N during the water draw and the reused texture projects correctly = no ghost, and because
the texture+projection are both tick N the layer is byte-identical across the field pair = no
flicker. **This is exactly `water_rendering.md` strategy (b1) / `interp_screenspace_strategy.md`
§3:** the water (and every EFB-feedback effect) runs at native 30fps, held to tick N, while the
opaque geometry interpolates at 60fps via the registry blend.

The **tension** noted in those docs stands: the water is drawn in the **same `gfx`** the opaque
scene uses, and the opaque scene wants `gfx+0xB4` *interpolated*. So you cannot leave `gfx+0xB4`
globally frozen — you must blend it for the opaque/geometry lists and **restore it to tick N only
around the water/post pass** (a scoped save/restore bracketing the 0x24 list, mirroring the
existing `saved_jview` restore at :318-320 but applied to `gfx+0xB4` and `gpCamera->mFovy/mAspect`
instead of `j3dSys`). That scoped restore is implementable and is the recommended fix.

**If the scoped restore proves leaky** (e.g. the water's `& 4` eye-space-quad build shares a list
with opaque geometry that must stay interpolated, so you cannot freeze `gfx+0xB4` for one without
the other): fall back to the **30fps water bypass** — do **not** re-issue the water/post pass on
the in-between at all (drop list `0x24` from `kDrawLists`, and never re-run the water object's
`& 4`/`& 0x80` on the in-between). The water then simply isn't redrawn on the in-between; the VI
re-presents the real field's water region (single-XFB, present-to-alt) → 30fps water, 60fps
geometry, no ghost, no flicker — the honest hardware-faithful cadence.

**Detection signal for "water is on screen" (to choose 30fps-bypass per scene):** the water
manager's surface flags `unk5D60` — refraction draws iff `unk5D60 & 2` (line 1503). Probe the
live `TModelWaterManager` instance's `unk5D60`; equivalently, count `drawRefracAndSpec` issuance
(an override tee on `0x8027c12c`) — nonzero in the last real field ⇒ water on screen ⇒ skip the
in-between water re-issue. The deep-water layer adds `TMapObjWaterFilter::perform` activity
(MapObjWater.cpp:47, gated on `gpMapObjWave`/`gpCamera->unk124.y`) as a secondary signal.

### Flagged / uncertain
- `gpCamera`'s runtime data address is SDA/r13-relative (the funcs file lists code only); resolve
  it the way `gpMarioPos` is resolved in interp_redraw.cpp:265 (r13 + signed offset) and verify by
  write-watch, then freeze `mFovy`@+0x48 / `mAspect`@+0x4C.
- Which perform LIST issues the water `& 4` (eye-space quad build) vs the `& 0x80` (draw) is
  data-driven (`PerformLists.bin`); confirm at runtime before assuming the `gfx+0xB4` freeze window
  = only the 0x24 list. If `& 4` is in an earlier list, the freeze (or the bypass) must cover it.
- `SMS_GetLightPerspectiveForEffectMtx` (deep-water / shimmer effect texmtx) is not in the
  vendored decomp; assumed camera-derived (fed `param_2`/`gpCamera`, not `j3dSys`). Verify if the
  deep-water J3D layer is the visible flicker rather than the `TModelWaterManager` surface.
