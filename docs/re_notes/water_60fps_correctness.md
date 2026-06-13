# Water layer 60fps correctness — why it flickers when the opaque scene is a clean midpoint

Scope: the user's new, sharper observation supersedes the earlier "render the water at
30 fps, it's faithful" conclusion in `interp_screenspace_strategy.md`. A per-pixel diff of
the real field vs the in-between field shows the OPAQUE scene (buildings, ground) is a
CLEAN near-identical midpoint, but the WATER surface + the shadow projected on it differ a
lot and read as FLICKER (not smooth motion). User goal: 60 fps graphics / 30 fps logic,
and the water must be a correct 60 fps midpoint like the opaque scene — NOT a 30 fps
fallback.

All addresses GMSE01 (`reference/sms_gmse01_funcs.txt`); file:line into `reference/sms/`.
Runtime: `runtime/overrides/interp_redraw.cpp`, `interp_capture.cpp`. Cross-refs:
`water_rendering.md`, `water_shadow_determinism.md`, `direct_draw_flow.md`,
`j3d_draw_pipeline.md`, `interp_screenspace_strategy.md`.

---

## 0. TL;DR conclusion (read this first)

**The opaque scene is a clean midpoint because its geometry is interpolated through the
`mDrawMtxBuf` blend (`interp_capture.cpp`), which feeds the GPU's indexed pos-matrix loads
directly. The water layer is NOT, and CANNOT be, interpolated by that same mechanism —
because the water surface is drawn from a SEPARATE, NON-double-buffered, world-space quad
buffer (`unk5D30`, a `TDLTexQuad`) projected per-draw through the *live camera*, and its
two governing per-field inputs — (a) the camera projection texmtx and (b) the EFB it samples
+ the EFB dst-alpha/Z mask it depends on — are advanced INCONSISTENTLY on the in-between:**

- The current code restores the **tick-N** camera before the GXPost (water) pass
  (`interp_redraw.cpp:318-320`, the `freeze_water` path) so the water samples the reused
  screen texture without ghosting. So on the in-between, the water is drawn with the **tick-N
  camera projection over the interpolated-camera opaque EFB**. The opaque scene moved (60 fps
  blend) but the water's screen-space projection did NOT — they disagree → the refracted/
  reflected content slides under a stationary-projected surface → shimmer/flicker.
- Meanwhile `drawWaterVolume`/`drawShineShadowVolume` write the **EFB destination-alpha mask
  and Z** that `drawRefracAndSpec` and the shadow-on-water test against. Those `& 8`/`& 80`
  passes ARE re-issued on the in-between, but they run against a **different Z/EFB state**
  than the real field (interpolated opaque Z on the in-between, tick-N Z on the real field),
  so the dst-alpha-gated water body and the shadow alternate coverage → the flicker.

**The precise reason:** the water is the ONE layer whose per-field state is split across two
non-interpolated channels (its own world-space quad projected by a frozen-vs-live camera,
and the EFB dst-alpha/Z mask) that the in-between advances out of lockstep with the opaque
geometry. The fix to make it a correct 60 fps midpoint is **NOT** to freeze it at 30 fps —
it is to make EVERY water input use the SAME interpolated camera/view the opaque scene used,
AND re-run the water's own `& 4` vertex/matrix calc and `& 8` dst-alpha/Z mask passes against
that same interpolated state, so the whole water sub-pipeline is self-consistent with the
interpolated EFB. See §5.

This contradicts the prior `interp_screenspace_strategy.md` recommendation (b1: freeze the
water at tick N). That recommendation produces exactly the mismatch the user now sees
(frozen surface projection over moved geometry). **Flagged: that doc's conclusion is
falsified by the user's per-pixel diff and should be treated as superseded by this one.**

---

## 1. Every per-field input to `drawRefracAndSpec` (0x8027c12c) + `perform` (0x8027beb0),
## re-examined for CONSISTENCY between the real field and a correct in-between

`TModelWaterManager::perform` (ModelWaterManager.cpp:1541) is phase-multiplexed. The phases
and WHERE they run (per `direct_draw_flow.md` §2-3):

| phase | body | runs in list | re-issued on in-between? |
|---|---|---|---|
| `& 1` | `move()`, `calcWorldMinMax()`, `unk5E00++` (:1545-1549) | `unk34`/calc | **NO** (correct — 30 Hz sim) |
| `& 4` | `calcDrawVtx()` (EMPTY, :763) + **`calcVMAll(viewMtx)`** (:1551-1560) | `unk34` filter 0x4 | **NO** ← see §1a |
| `& 8` | `drawSilhouette`, **`drawWaterVolume`**, `drawMirror`, `drawShineShadowVolume` (:1562-1578) | GX/Silhouette (0x1C/0x20) | **YES** |
| `& 80` | **`drawRefracAndSpec`** + `drawShineShadowVolume` (:1580-1591) | GXPost (0x24) | **YES** |

### 1a. The water surface quad `unk5D30` is world-space, NOT double-buffered, projected by the live camera

`drawRefracAndSpec` (ModelWaterManager.cpp:1442) draws `unk5D30->draw()` (:1504, :1522,
:1538). `unk5D30` is a `TDLTexQuad` (created :118-119, `createBuffer(256)`). Its `draw()`
(DLUtil.cpp:97-115) emits an indexed quad list whose POSITION array is `unk14[unk4]` —
**raw world-space Vec positions** (`request()` stores 4 Vecs per quad, DLUtil.cpp:48-51) —
with **identity PNMTX0** (drawRefracAndSpec :1446-1448 loads identity into GX_PNMTX0). So
the water surface vertices are world-space and STATIC within a frame; they are NOT in a
J3DModel `mDrawMtxBuf` and are therefore **completely outside the `interp_capture.cpp`
blend**. The opaque scene interpolates because its matrices live in `mDrawMtxBuf[1][view]`
(j3d_draw_pipeline.md §2-5); the water surface does not, so the registry blend (mode 3,
`interp_redraw.cpp:311`) never touches it.

The water's on-screen motion comes ENTIRELY from the projection texmtx:

```
GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_POS, 0x1e, 0, 0x7d);   // :1454  POS -> texmtx 0x1e
C_MTXLightPerspective(m, gpCamera->getFovy(), gpCamera->getAspect(), 0.5f,-0.5f,0.5f,0.5f);  // :1457
GXLoadTexMtxImm(m, 0x1e, GX_MTX3x4);                                        // :1459
```

`C_MTXLightPerspective` (0x8034a17c) builds a screen-projection matrix from the **live
camera fovy/aspect**. So the screen-space UV where each (static) water vertex samples the
screen texture = the camera projection of that vertex. **This is the per-field handle for
the surface.** If the camera is interpolated, the projection follows; if frozen at tick N,
the surface projects to its tick-N screen position.

> **The split that causes the flicker:** the OPAQUE scene's screen position is driven by the
> interpolated `mDrawMtxBuf` (60 fps). The WATER surface's screen position is driven by the
> camera projection texmtx (`C_MTXLightPerspective` of `gpCamera`). For them to agree on the
> in-between, the camera the water uses MUST be the SAME interpolated camera the opaque
> geometry's `viewCalc` baked in. Today they DON'T agree — see §1c.

### 1b. The refraction screen texture (EFB copy) — consistency requirement

`unk5D34` (the "スクリーンテクスチャ" EFB copy) is bound GX_TEXMAP0 (:1474). It is filled by
the "通常シーン描画ステージ" `TEfbCtrlTex::perform` `GXCopyTex` (0x802f8bac, copy bit `& 0x8`).
On the in-between the current code DROPS that copy (`interp_redraw.cpp:112-118`,
`ov_efbctrltex_perform` when `freeze_water`/`skip_efbcopy`), so the water samples the REAL
field's screen texture.

For a CORRECT midpoint this is the wrong choice. The opaque EFB on the in-between IS the
interpolated scene (the diff confirms it's a clean midpoint). The screen texture SHOULD be
re-copied from that interpolated EFB so the reflection reflects the interpolated world —
exactly as the opaque scene is itself interpolated. The earlier "reuse the real copy →
ghost" failure (`interp_screenspace_strategy.md` §2) happened because the surface
*projection* was interpolated while the content was frozen; the symmetric correct fix is to
interpolate BOTH (re-copy the EFB AND project with the same interpolated camera), not to
freeze both.

### 1c. The camera the water actually uses on the in-between is INCONSISTENT with the opaque scene

This is the live bug. `interp_redraw.cpp`:
- Blends the j3dSys view matrix (`J3DSYS_VIEWMTX` 0x804045DC) toward N-1 for the redraw
  (:243-248) so the OPAQUE geometry's `viewCalc` recompute and the view node use the
  interpolated camera.
- But then, **before the GXPost (0x24, water) pass, restores the view to tick N**
  (`freeze_water` path, :318-320): `for (i<12) mem_wf32(J3DSYS_VIEWMTX..., saved_jview[i]);`

So on the in-between: opaque scene = interpolated camera; water `drawRefracAndSpec` =
tick-N camera (`gpCamera->getFovy/getAspect` are read live, but the j3dSys view used to
build the water quads in `& 4`/the view node is restored to N). The water surface projects
to its tick-N screen location over an opaque EFB that has moved to the midpoint → the
refracted content under the surface is offset from the surface = the shimmer/flicker the
user sees. **This restore is the concrete defect for the SURFACE half of the flicker.**

> Note: `gpCamera->getFovy()/getAspect()` (the projection scale) barely change frame to
> frame; the dominant per-field term is the **view (eye) matrix**, which determines where in
> screen space each water vertex lands. The j3dSys view restore (:318-320) is what freezes
> that.

### 1d. Frame-counter animation — NOT the flicker, but must stay single-stepped

`unk5E00` (:1548) and any `J3DFrameCtrl`/SRT scroll advance only under `& 1`, which the
in-between does not run. So both fields land on the same phase (tick N). This produces a
half-frame hold (judder), NOT a per-field INCONSISTENCY, so it is not the flicker source
(confirmed: a flicker needs the two fields to DIFFER; a frozen counter makes them the SAME).
`drawRefracAndSpec` itself reads no frame counter (water_shadow_determinism.md §1). Keep
`& 1` single-stepped (re-running it would double-advance `unk5E00` and any scroll = a real
texture jump).

---

## 2. Is the flicker CONTENT or BUG? Both — and the BUG dominates

### 2a. The CONTENT component (legitimate, small)
If we re-copied the interpolated EFB (we currently don't), the reflection content would
LEGITIMATELY differ between the real and in-between fields — but so does the opaque scene,
and the user accepts that as a clean midpoint. A correctly interpolated reflection of a
correctly interpolated scene is itself a clean midpoint. This is NOT flicker; it is the
intended 60 fps motion. (Half-res RGB565 screen texture, low frequency — the midpoint of two
midpoint-scenes is smooth.)

### 2b. The BUG component (dominant — three distinct mismatches)

1. **Surface projected by a frozen camera over a moved EFB (§1c).** The water surface's
   screen position is locked to tick N (`freeze_water` view restore, :318-320) while the
   opaque EFB it refracts is the interpolated midpoint. Surface and content disagree → the
   reflected pixels slide under the surface every other field. **Primary surface flicker.**

2. **Stale reused screen texture (§1b).** With `skip_efbcopy`/`freeze_water`, the in-between
   reflects the REAL field's EFB, not the interpolated one. Even with the projection fixed,
   the reflected content would be one field stale relative to the surface → residual mismatch
   on moving reflected objects.

3. **EFB dst-alpha + Z mask built against an inconsistent scene = INCOMPLETE/DIFFERENT EFB
   for the water (the §3 layer-completeness issue).** This is the shadow-on-water half.

### 3. The EFB the water depends on is built differently on the in-between (layer completeness)

The water surface is NOT a simple textured quad — it is **dst-alpha-masked and Z-tested
against the EFB**, and that mask is written by passes whose result depends on the field's Z
buffer:

- `drawWaterVolume` (:997, `& 8`) calls `SMS_FillScreenAlpha(0)` (:999) which writes the EFB
  **destination-alpha** with `GXSetDstAlpha(GX_TRUE, ...)` + `GXSetZMode(GX_TRUE, GX_ALWAYS)`
  (ScreenUtil.cpp:252-258), then draws the water body cubes with `GX_BL_DSTALPHA`/
  `GX_BL_INVDSTALPHA` blends (:1064, :1086, :1146) and `GXSetZMode(GX_TRUE, GX_LEQUAL)`
  (:1148) — i.e. the water body's coverage is **gated by the EFB depth buffer** (where opaque
  geometry occludes the water) and writes a dst-alpha stencil.
- `drawRefracAndSpec` then draws the refraction quad with `GXSetZMode(GX_TRUE, GX_LEQUAL,
  GX_FALSE)` (:1499) and `GXSetZCompLoc(1)` (:1473) — **Z-tested against the same EFB depth**.
- `drawShineShadowVolume` (:1310, the shadow ON the water) writes dst-alpha
  (`GXSetColorUpdate(GX_FALSE)`/`GXSetAlphaUpdate(GX_TRUE)`, :1367-1368) using
  `param_2->mViewMtx` and runs in `& 8` or `& 80`.

On the REAL field these passes test/write against the **tick-N opaque Z buffer**. On the
in-between they are re-issued (0x1C/0x20/0x24 ARE in `kDrawLists`), but the opaque geometry
that established the Z buffer was drawn with the **interpolated** `mDrawMtxBuf` — so the EFB
depth (and thus where the water body and shadow pass the Z/dst-alpha test) is at the
**interpolated** state, while the water surface QUAD is projected at **tick N** (§1c). The
dst-alpha-masked water body + the Z-tested refraction + the projected shadow therefore cover
a DIFFERENT set of pixels on the in-between than on the real field — and because the surface
projection is frozen but the Z mask moved, the boundary between "water visible" and
"occluded by geometry" jitters every other field. **This is the water+shadow in-sync
flicker.** It is a real per-field INCONSISTENCY (frozen surface vs moved Z mask), not just
content motion — exactly the "missing/incomplete EFB layer" class the task asked to
enumerate, here manifested as a Z/dst-alpha MISALIGNMENT rather than a missing draw.

Enumerated EFB-coupled inputs that the in-between currently advances out of lockstep:
- EFB **color** (the screen texture) — reused stale (§1b), surface projected frozen (§1c).
- EFB **depth (Z)** — at the interpolated state (opaque drawn interpolated), but the water
  surface tests it with a tick-N projection → boundary jitter.
- EFB **dst-alpha** — written by `SMS_FillScreenAlpha`/`drawWaterVolume`/
  `drawShineShadowVolume` against the interpolated Z, then consumed by the water body and
  the shadow → coverage alternates.

No water layer is entirely MISSING from the in-between EFB (the `& 8`/`& 80` passes do
re-run); the defect is that they run with a **frozen surface projection over a moved
Z/EFB**, so the layers are present but MISALIGNED.

---

## 4. Why the opaque scene is clean but the water is not (the asymmetry, precisely)

| | Opaque scene | Water layer |
|---|---|---|
| Geometry source | J3DModel/SDLModel `mDrawMtxBuf[1][view]` (double-buffered) | `unk5D30` `TDLTexQuad`, world-space, single buffer |
| Interpolated by | `interp_capture.cpp` registry blend (60 fps) — feeds GPU indexed pos-mtx load directly | NOTHING — outside the registry |
| Screen position driven by | the blended draw matrix (interpolated view×node) | `C_MTXLightPerspective(gpCamera)` projection texmtx |
| Camera used on in-between | INTERPOLATED j3dSys view (`interp_redraw.cpp:243-248`) | **tick N** (view restored at :318-320) |
| EFB dependency | writes color/Z | SAMPLES color (screen tex), TESTS Z, reads/writes dst-alpha |

The opaque scene is a clean midpoint precisely because the one thing that determines its
screen position (the draw matrix) is interpolated and fed straight to the GPU. The water
fails on three counts the opaque scene doesn't have: (1) its surface position is NOT in a
double buffer so the registry blend can't reach it, (2) the code deliberately freezes the
camera for it, and (3) it has a non-linear EFB feedback dependency (sample color, test Z,
gate on dst-alpha) that must be re-derived consistently, not frozen.

---

## 5. CONCLUSION — the fix to make the water a correct 60 fps midpoint

**The water should be interpolated the SAME way the opaque scene is: with the interpolated
camera, re-running its own per-field calc, against the interpolated EFB. Stop freezing it.**

Concretely, on the in-between field:

1. **Do NOT restore the tick-N j3dSys view before the GXPost/water pass.** Delete /
   condition-off the `freeze_water` view restore at `interp_redraw.cpp:318-320`. The water's
   `C_MTXLightPerspective` and any view-matrix-fed water calc must use the SAME interpolated
   j3dSys view (0x804045DC) the opaque geometry used. This aligns the surface's screen
   projection with the interpolated EFB it samples and Z-tests against — killing the §1c /
   §3 surface-vs-content slide. **This is the load-bearing change.**

2. **Re-run the water's `& 4` calc on the in-between against the interpolated view.** The
   water surface quads / particle matrices (`calcVMAll`, :1556) and the `unk5E10` view copy
   (:1572) are built from `param_2->mViewMtx`. `& 4` lives in `unk34` (filter 0x4), which the
   in-between does NOT re-issue (`direct_draw_flow.md` §3). Either re-issue the water object's
   `& 4` phase with the interpolated view, OR (cheaper) since the refraction surface quad is
   world-space-static and only its PROJECTION matters, ensure the interpolated view reaches
   `gpCamera`/j3dSys so `drawRefracAndSpec`'s `C_MTXLightPerspective` and the `& 8` Z/dst-alpha
   passes all use it. Do NOT re-run `& 1` (would double-step `unk5E00`/scrolls, §1d).

3. **Re-copy the screen texture from the interpolated EFB** (do NOT `skip_efbcopy` on the
   in-between). Let `TEfbCtrlTex::perform`'s `& 0x8` copy run so `unk5D34` holds the
   interpolated scene. With (1) fixing the projection, the reused-texture ghost (the original
   reason `skip_efbcopy` was added) no longer applies — the surface and the content are both
   at the interpolated state, so they agree. This makes the reflection a true midpoint of two
   midpoint-EFBs (the §2a legitimate content interpolation), matching the opaque scene.

4. **Ensure the EFB Z/dst-alpha mask passes (`& 8`: `drawWaterVolume`, `drawShineShadowVolume`)
   run against the interpolated camera/geometry** — they already re-issue (0x1C/0x20), so once
   (1) removes the camera freeze they will test the interpolated Z with the interpolated
   projection, aligning the water-body coverage and the shadow-on-water with the surface.
   This fixes the shadow half of the in-sync flicker.

Net: every water input (surface projection, sampled color, tested Z, dst-alpha mask, shadow
projection) is then driven by the ONE interpolated camera/EFB the opaque scene already uses
→ the water becomes a clean 60 fps midpoint, not a frozen 30 fps layer. This is the
faithful-to-the-engine path: the engine itself draws the water as a pure function of
(camera, EFB, world-space surface) — feed it the interpolated camera and interpolated EFB
and it produces the interpolated water for free.

### Why the prior "30 fps water is faithful" recommendation is wrong here
`interp_screenspace_strategy.md` §0/§3 recommended (b1): freeze the screen texture AND the
surface verts at tick N. That is precisely the current `freeze_water` code, and it produces
the user's reported flicker (frozen surface projection over a moving opaque EFB) — because
the opaque scene around/under the water DOES move at 60 fps, so a frozen water surface
disagrees with it every field. The earlier doc assumed "hold both → they agree", but it
overlooked that the water is composited OVER and Z-tested AGAINST the 60 fps opaque scene,
so freezing the water freezes only HALF the comparison. The correct symmetry is to
interpolate both halves, not freeze both. **Treat `interp_screenspace_strategy.md`'s (b1)
conclusion as superseded by this doc.**

---

## 6. Flagged / uncertain

- **Camera-projection vs view split.** `drawRefracAndSpec` reads `gpCamera->getFovy()/
  getAspect()` LIVE (:1457) for the projection SCALE, but the per-vertex screen POSITION
  comes from the view (eye) transform via `GX_TG_POS` → the loaded PNMTX/view. The dominant
  per-field term is the view matrix (j3dSys 0x804045DC), which the `freeze_water` restore
  freezes. Confirm at runtime that `gpCamera`'s own view (not just fovy/aspect) is what the
  POS texgen resolves through — i.e. that interpolating j3dSys view is sufficient and we do
  not ALSO need to interpolate a separate `gpCamera` view used elsewhere. (`gpCamera` is a
  distinct object from j3dSys; both may carry a view — verify which one `drawRefracAndSpec`'s
  texgen depends on by probing whether the surface moves when j3dSys view alone is blended.)
- **`& 4` re-issue cost/safety.** Re-running `calcVMAll` (:1556) on the in-between rebuilds
  particle matrices `unk2D14[]` from the interpolated view. This is idempotent (overwrites,
  reads no frame counter) and desirable, but it lives in `unk34` (not currently re-issued).
  Re-issuing just the water object's `& 4` (not the whole `unk34`) requires identifying the
  water link; the perform-list membership is data-driven (`PerformLists.bin`). If re-issuing
  `& 4` is awkward, option (2)'s cheaper variant (rely on the world-space quad + interpolated
  projection) likely suffices for the refraction surface, since `drawRefracAndSpec` re-reads
  the live camera each draw and the quad positions don't depend on the view.
- **`drawShineShadowVolume`'s static `tmp_data` buffers** (:1316-1318) are map-1-specific
  and use hardcoded matrices (:1340-1341) plus `param_2->mViewMtx`; verify the shadow volume
  tracks the interpolated view once the freeze is removed. It is only active for
  `getCurrentMap() == 1` (:1313).
- The exact `unk5D60` surface flags (`& 2/4/8`, refraction/water-color/spec sub-pass gating)
  are load-time constants (water_shadow_determinism.md §1, `unk5D60 = 367`), so the sub-pass
  set does not change per field — not a flicker source, but confirm at runtime in the plaza
  specifically.
- This doc is RE/analysis only; the §5 fix is a design, not yet implemented or verified.
  Verify headed: after removing the freeze + re-enabling the EFB re-copy, the per-pixel diff
  of the water region should become as clean a midpoint as the opaque scene.
