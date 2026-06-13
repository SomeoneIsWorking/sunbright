# Sun / specular / EFB-feedback effects — per-field hazard analysis

Reverse-engineered from `reference/sms/` for the 60fps in-between-field re-issue path.
The 60fps path re-issues the GX draw pass on an in-between field **without** re-running
the 30Hz calc/movement perform-lists. Effects that read back the EFB/Z-buffer mid-frame
to gate a draw are therefore hazardous: re-issuing them samples a *different* EFB and may
flip the visibility decision; skipping them makes them appear only on real fields.

This is documentation only. No code changed.

---

## 0. How the EFB readback is wired (the shared mechanism)

The game does its framebuffer feedback through GX **draw-sync tokens** + a draw-sync
callback, NOT inline peeks. The flow, per frame:

1. A `TZBufferCatch` / `TAlphaCatch` view-object sits in a draw perform list. In its
   draw phase (`perform` `&8`) it pushes a breakpoint and emits a draw-sync token into
   the GPU FIFO:
   - `reference/sms/src/System/ZBufferCatch.cpp:6` `TZBufferCatch::perform` → `GXSetDrawSync(0x7D)`  (addr **0x802a58ec**)
   - `reference/sms/src/System/ZBufferCatch.cpp:15` `TAlphaCatch::perform` → `GXSetDrawSync(0x7C)` (and a trailing `0x0`) (addr **0x802a58a8**)
   These run AFTER the opaque scene/objects that occlude the sun / Mario have been drawn,
   so the EFB/Z at that point reflects "what is in front."

2. When the GPU reaches the token it raises a draw-sync interrupt; Dolphin/HW calls the
   registered `GXSetDrawSyncCallback` → `TDrawSyncManager::drawSyncCallback`
   (`reference/sms/src/System/DrawSyncManager.cpp:61`), which dispatches by token range to
   the registered callbacks. Registration (`reference/sms/src/System/MarDirectorSetup2.cpp:91`):
   - token **0x7D** → `gpSunMgr`            (sun Z-occlusion probe)
   - token **0x7C** → `gpMarioOriginal`     (Mario alpha-occlusion probe)
   - tokens 0x7E..0xA5 → pollution counter layers (not in scope here)

3. The callbacks do the actual EFB peek:
   - `TSunMgr::drawSyncCallback` (`sunmgr.cpp:106`, addr **0x8002e270**) → `gpSunModel->getZBufValue()`.
   - `TMario::drawSyncCallback` (`MarioMain.cpp:249`, addr **0x8024d17c**) → `GXPeekARGB`.

**Crucial timing fact:** the peek reads the EFB *as drawn so far this frame*. The
visibility result is stored in object fields and consumed by a LATER draw (this frame for
the sun glints, next frame's setup for the smoothed alpha). The decision is therefore a
property of *what was rendered into the EFB before the token*, not of the calc tick.

---

## 1. Sun / lens effects

All four objects are created in `reference/sms/src/System/MarDirectorInitECT.cpp` (the
"…Mir"/mirror-scene init, `initECTMir`) and `MarDirectorSetupObjects.cpp`. They are split
across perform lists by phase: their **calc (`&1`) / anim (`&2`)** run in the 30Hz
movement & calc-anim lists; their **GX entry (`&0x200`) / viewCalc (`&4`)** run in the GX
draw list (`mPerformListGX`, via `initECTMir`, `MarDirectorSetupObjects.cpp:385`).

### 1a. TSunModel — "太陽モデル" (the sun disc + the Z-occlusion probe driver)
- Source: `reference/sms/src/Camera/sunmodel.cpp`; header `include/Camera/SunModel.hpp`.
- `getZBufValue` base **0x8002ea70**; the inner GXPeekZ loop body is **0x8002fdbc**.
- **What it draws:** the sun billboard model (`unk48`, a `J3DModel`) and a mirror copy
  ("太陽in鏡"). The draw itself does NOT read the EFB.
- **EFB read:** `TSunModel::getZBufValue` (`sunmodel.cpp:265`) loops over **17 screen
  points** (`unkB4[17]`, a center + two rings of 8 computed in
  `calcDispRatioAndScreenPos_`, `sunmodel.cpp:149`) and for each does
  `GXPeekZ(x, y, &z)`; if `z == 0xffffff` (far plane = nothing drawn there) it marks that
  point **visible** (`unk180[i] = 1`). So the sun is "visible" only where the depth buffer
  is still cleared — i.e. unoccluded sky.
- **How it gates the draw:** the count of visible points → `unk191` (count) and `unk194`
  (`= count/17`, the "dispersion ratio") in `calcDispRatioAndScreenPos_`. In
  `TSunModel::perform` `&1` (`sunmodel.cpp:189`) those drive the sun's TEV alpha
  (`unk8C.color.a`, `unk94.color.a` via `CLBLinearInbetween`/`CLBEaseOutInbetween` +
  `CLBChaseDecrease` smoothing) and the additive glow `unkAC` (`getAddColor`). The smoothing
  (`CLBChaseDecrease`/`CLBChaseGeneralConstantSpecifySpeed`) means the visible-point count
  feeds an **alpha ramp**, not a hard on/off.
- It is also the **data source for the other three effects** (they read
  `gpSunModel->unk194`, `unk191`, `unkF8[]`, `unk180[]`, `isInBounds`).

### 1b. スペキュラシーン — "specular sheen"
- Searched by name in `MarDirectorInitECT.cpp:110` and `MarDirectorSetupObjects.cpp:445`.
  It is a **data-defined view object** (loaded from the scene `.bin`; there is no
  `TSpecularSheen` class in `src/`). I could not pin its concrete class/source — **FLAGGED:
  unresolved class**. It is registered into `mPerformListMovement` (phase 1,
  `MarDirectorSetupObjects.cpp:447`) and `mPerformListCalcAnim` (phase 2, line 468), and
  into the GX list with flag `0x204` (`MarDirectorInitECT.cpp:166`) — i.e. a normal calc +
  GX-entry view object. It draws the screen-space sheen/streak overlay scaled by the sun's
  dispersion. Behaviorally it consumes `gpSunModel`'s readback result (the dispersion
  ratio) rather than peeking the EFB itself — same class as the lens flare/glow below.

### 1c. レンズフレア — TLensFlare ("lens flare")
- Source: `reference/sms/src/Camera/lensflare.cpp`; `perform` addr **0x8002d154**.
- **What it draws:** the `sun_lensfx.bmd` flare model along the screen line from the sun
  toward screen center (`perform` `&2` computes the position from a near-plane 9-grid;
  `&0x200` entries the model).
- **EFB read:** none directly. It reads `gpSunModel->calcHiddenRatio()`
  (`lensflare.cpp:58`) and `gpSunModel->getUnk194()` — i.e. it consumes the sun's
  Z-occlusion result.
- **Gate:** `unk28 = CLBEaseOutInbetween(unk48*(1-hiddenRatio), 255, dispRatio)`
  (`lensflare.cpp:60`), smoothed into the material alpha `unk24`. Alpha ramp, occlusion-
  driven, computed in phase `&1` (calc tick).

### 1d. 太陽遮蔽物グロー — TLensGlow ("sun occlusion glow")
- Source: `reference/sms/src/Camera/lensglow.cpp`; `perform` ~**0x8002cxxx** (in the
  `TLensGlow` block of funcs.txt; exact line not enumerated above — FLAGGED minor).
- **What it draws:** the `glow.bmd` halo at the sun's screen position.
- **EFB read:** none directly. Consumes `gpSunModel->getUnk194()` (dispersion),
  `getUnk191()` (visible-point count), and `gpSunModel->unk180[]`/`unkF8[]` to compute the
  glow center as the average of the *visible* sample points (`lensglow.cpp:135-166`).
- **Gate:** target alpha `unk4C` from the visible count vs `unk5D` threshold, smoothed
  into `unk48` → material alpha (`lensglow.cpp:189`). Alpha ramp, occlusion-driven,
  computed in phase `&1`.

**Net for the sun cluster:** exactly ONE EFB read in the whole cluster — `TSunModel::
getZBufValue` (GXPeekZ ×17). The other three effects are pure consumers of its result.

---

## 2. Mario occlusion silhouette (Mario darkened behind objects)

Two cooperating pieces: the EFB **alpha** probe (MarioMain) and the smoothed alpha
feedback + redraw (DrawUtil `TSilhouette`).

### 2a. The probe + alpha stamping — `reference/sms/src/Player/MarioMain.cpp`
Inside `TMario::draw` (the big phase-switched draw), gated on
`UNK114_FLAG_DO_OCCLUSION_PROBE` (=0x400, `Mario.hpp:1591`):
- phase `&0x02000000` (`MarioMain.cpp:193`): draw a bounding cube with color update OFF,
  alpha update ON, `GXSetDstAlpha(GX_ENABLE, 0x10)` → **stamps EFB alpha = 0x10** in
  Mario's silhouette footprint. This is the "Mario was here" marker.
- phase `&0x00800000` (`MarioMain.cpp:206`): same cube with `GXSetDstAlpha(GX_ENABLE, 0)` →
  re-stamps alpha 0 (cleanup / for the parts now occluded).
- The probe: `TMario::drawSyncCallback` (`MarioMain.cpp:249`, addr **0x8024d17c**), fired
  by token **0x7C** (`AlphaCatch`). It `GXPeekARGB(mMarioScreenPos.x, y, &argb)` at Mario's
  screen anchor and tests `(argb & 0xff000000) == 0x10000000`:
  - alpha still 0x10 → nothing was drawn over that pixel after the stamp → **not occluded**
    → `offFlag(MARIO_FLAG_OCCLUDED)`.
  - alpha != 0x10 → an opaque object overwrote it → **occluded** → `onFlag(MARIO_FLAG_OCCLUDED)`.
  (Off-screen anchor → forced not-occluded, `MarioMain.cpp:254`.)
- phase `&0x80000000` (`MarioMain.cpp:219`): the silhouette redraw — draws Mario's model
  (`unk394`/`unk398`) with `GXSetDstAlpha(GX_ENABLE, gpSilhouetteManager->unk48)` and
  `GX_BM_BLEND, GX_BL_DSTALPHA, GX_BL_INVDSTALPHA`, i.e. blends a dark silhouette weighted
  by the dst-alpha that the probe path established. `gpSilhouetteManager->unk48` is the
  smoothed occlusion alpha (see 2b).

### 2b. The feedback loop — `reference/sms/src/MarioUtil/DrawUtil.cpp` `TSilhouette`
- `TSilhouette::perform` addr **0x80227914**.
- phase `&1` (calc, `DrawUtil.cpp:95`):
  `fVar1 = MARIO_FLAG_OCCLUDED ? unk50 : 0.0f` (`unk50 = 128.0f`);
  `unk48 += unk4C * (fVar1 - unk48)` (`unk4C = 0.01f`) — a **low-pass filter** that ramps
  the silhouette alpha toward 128 when occluded, toward 0 when not. `unk12.a = unk48`.
- phase `&8`/`&0x80` (`DrawUtil.cpp:101,110`): `setting()` (`DrawUtil.cpp:68`, addr
  0x80227d0c) installs a spot light from `gpMarioPos` whose color alpha = `unk48`, used to
  shade the ground darkening.
- Consumers of `unk48`/`MARIO_FLAG_OCCLUDED`:
  - `CameraNormal.cpp:160` (camera reaction to occlusion),
  - `ModelWaterManager::drawSilhouette` (`ModelWaterManager.cpp:981,989`),
  - `question.cpp:95,105`,
  - the MarioMain redraw above.

**The loop:** EFB alpha probe (this frame) → `MARIO_FLAG_OCCLUDED` → `TSilhouette::perform`
`&1` low-passes it into `unk48` (per calc tick) → next draw uses `unk48` as the silhouette/
ground-darken alpha. The hard EFB decision is filtered into a slowly-ramping alpha.

---

## 3. Marukage (round drop shadow under Mario)

- `TSilhouette::perform` `&0x10` branch (`DrawUtil.cpp:116`). It **sets up GX state but
  draws no geometry of its own**:
  - builds a projective light-frustum texmtx from `gpMarioPos`
    (`C_MTXLightFrustum` + rot/scale/translate by `-gpMarioPos->x/z`, `DrawUtil.cpp:117-132`),
  - `GXLoadTexMtxImm(mtx, 0x1e, GX_MTX3x4)` (`DrawUtil.cpp:133`) — loads it as **texmtx 0x1e**,
  - `GXSetTexCoordGen2(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_POS, 0x1e, …)` (`DrawUtil.cpp:137`)
    — texcoord1 = vertex POSITION through texmtx 0x1e (the projector),
  - binds the pollution joint texture (TEXMAP0) and `H_marukage_xlu_i8.bti` (TEXMAP1,
    `DrawUtil.cpp:60,139`), sets a 2-stage TEV that modulates the shadow texture in.
- **What consumes it:** the geometry drawn *after* this object in the same perform list,
  with texgen src `GX_TG_POS` through texmtx `0x1e` (`= 0x1e`), i.e. the **ground / map**
  surface. The shadow is projected onto whatever ground geometry follows. Confirmed second
  consumer: **water** — `TModelWaterManager::drawSilhouette` (`ModelWaterManager.cpp:920`,
  addr **0x8027dd00**) rebuilds the same `GXLoadTexMtxImm(…, 0x1e, GX_MTX3x4)` from Mario's
  position (`ModelWaterManager.cpp:1230,1454-1459`) and renders the shadow onto the water
  surface. So the marukage is consumed by the ground draw (texmtx 0x1e left active by
  TSilhouette) AND by the water silhouette pass.
- `gpMarioPos = &gpMarioOriginal->mPosition` (`MarioAccess.cpp`), updated only at the 30Hz
  game tick. The projector therefore moves at 30Hz; the shadow draw, however, is part of
  the GX draw list that the 60fps path re-issues.
- The whole silhouette/shadow draw is gated by `gpSilhouetteManager->unk48 > 0` (or camera
  state) in the main loop — see §4.

---

## 4. Which perform list / phase each effect draws in

Main draw loop: `reference/sms/src/System/MarDirectorDirect.cpp` `direct()`.
- **Calc/movement (30Hz tick, NOT re-issued on in-between fields):**
  `mShinePfLstMov->perform` (`MarDirectorDirect.cpp:145`), `movement()` (line 153),
  `mPerformListCalcAnim->perform` (line 156). The sun/lens/specular `&1`/`&2` phases live
  here (`MarDirectorSetupObjects.cpp:442-475`): specularSheen, lensFlare, sunOcclusionGlow,
  TSunModel are pushed into `PerformList Movement` (flag 1) and `PerformList CalcAnim`
  (flag 2). `TSilhouette::perform &1` (the occlusion low-pass) also runs at this tick.
- **GX draw (re-issued on the in-between field):** the `else` branch
  (`MarDirectorDirect.cpp:166-177`):
  ```
  unk40->perform; unk38->perform; unk3C->perform;
  mPerformListGX->perform(0xffffffff)            // sun model, lens flare/glow, specular GX entry; ZBufferCatch
  if (gpSilhouetteManager->unk48 > 0 || gpCamera->unk2C8 != -1)
      mPerformListSilhouette->perform(0xffffffff) // TSilhouette setup + marukage + Mario silhouette geometry; AlphaCatch
  mPerformListGXPost->perform(0xffffffff)
  ```
  `mPerformListGX` is built by `initECTMir` (`MarDirectorSetupObjects.cpp:385`) and is where
  `TZBufferCatch` (`GXSetDrawSync 0x7D`) sits — so the **sun Z-probe token is emitted every
  time the GX list is issued**, including in-between fields. `mPerformListSilhouette` holds
  the Mario silhouette + marukage + `TAlphaCatch` (`GXSetDrawSync 0x7C`), so the **Mario
  alpha-probe token is also emitted on in-between fields**.
- `TSunMgr::drawSyncCallback`/`TMario::drawSyncCallback` fire whenever their token is
  consumed by the GPU — i.e. once per GX-list issue, real field or in-between.

---

## 5. CONCLUSION — per-field hazard + recommendation

| Effect | EFB read? | Where decided | Per-field hazard | Recommendation for the in-between field |
|---|---|---|---|---|
| **TSunModel Z-occlusion** (drives sun disc alpha + dispersion) | YES — `GXPeekZ`×17 in `getZBufValue` (0x8002ea70), fired by token 0x7D from `ZBufferCatch` in `mPerformListGX` | result → `unk180[]`,`unk191`,`unk194`; consumed by sun/lens/specular alpha next | **EFB-feedback, decision flips if re-issued.** Re-issuing the GX list re-emits token 0x7D → `getZBufValue` re-peeks the in-between EFB and overwrites `unk180/unk191/unk194` with a value the calc tick never smoothed → sun/glow/flare alpha jumps → **sparkle/glint flicker**. | Do NOT let the in-between re-run `getZBufValue`. Either suppress token 0x7D's callback on in-between fields, or snapshot `unk180/unk191/unk194` before the in-between and restore after, so the visible-point count carried forward equals the real field's. (Re-issuing the *draw* with the carried value is fine.) |
| **レンズフレア / 太陽遮蔽物グロー / スペキュラシーン** | NO direct read; consume TSunModel's result | their alpha ramps computed in `perform &1` at the 30Hz tick | **Draw-only on the in-between; safe as long as TSunModel's fields are stable.** Their alpha is set in a calc list that the in-between skips, so the alpha is simply the last real-field value (correct, no flip) — UNLESS TSunModel's `unk194` was corrupted by an in-between `getZBufValue` (the row above). | Re-issue the draw (they're in `mPerformListGX` and read fields only). The only fix needed is keeping TSunModel's readback fields stable (fix the row above). |
| **Mario occlusion silhouette** (Mario darkened behind objects) | YES — `GXPeekARGB` in `TMario::drawSyncCallback` (0x8024d17c), token 0x7C from `AlphaCatch`; also stamps EFB alpha 0x10 in the draw | probe → `MARIO_FLAG_OCCLUDED`; low-passed into `unk48` in `TSilhouette::perform &1` (0x80227914) at 30Hz | **EFB-feedback, decision flips if re-issued.** Re-issuing the GX/silhouette lists re-stamps alpha and re-fires token 0x7C → `MARIO_FLAG_OCCLUDED` is recomputed against the in-between EFB. The flag itself is consumed by the *next* calc tick's low-pass (`unk48`), so a stray in-between flip nudges `unk48` off the real-field trajectory → silhouette/ground-darken alpha flicker. | Carry forward the real field's visibility: do NOT let the in-between's `drawSyncCallback` write `MARIO_FLAG_OCCLUDED`, and keep `unk48` frozen across the in-between (it's only updated in `&1`, which the in-between skips — so the main risk is the flag write, suppress token 0x7C's callback on in-between fields). Re-issuing the silhouette *geometry* with the frozen `unk48` is correct. |
| **Marukage (round drop shadow)** | NO EFB read | projector texmtx 0x1e from `gpMarioPos` (30Hz); geometry = ground/water draw | **Draws in the GX/silhouette draw list → present on the in-between IF re-issued; missing if skipped.** No decision flips (no peek). The projector uses `gpMarioPos` (frozen at 30Hz), so on the in-between the shadow sits at the last real-field position — a half-frame of position lag, but no flicker. The known marukage flicker is the *gating* (`unk48 > 0`) coupling to the Mario occlusion `unk48` above, not the shadow geometry itself. | **Re-issue it** (it's pure geometry consuming a frozen texmtx). Keep `gpSilhouetteManager->unk48` frozen across the in-between so the `unk48 > 0` gate in `MarDirectorDirect.cpp:172` doesn't toggle the whole `mPerformListSilhouette` on/off between fields (that toggle = the shadow/water blink). Carrying `unk48` forward (per the Mario row) fixes both. |

### One-line summary
The flicker is driven by **two EFB reads** re-firing on the in-between field through
re-emitted draw-sync tokens: `GXPeekZ`×17 in `TSunModel::getZBufValue` (token 0x7D /
`ZBufferCatch`) for the sun/lens/specular sparkle, and `GXPeekARGB` in
`TMario::drawSyncCallback` (token 0x7C / `AlphaCatch`) for the Mario silhouette. Both write
visibility state that the 30Hz calc tick smooths; an in-between re-read corrupts that state.
**Recommendation:** on the in-between field, **suppress the draw-sync callbacks** (so
`getZBufValue`/`drawSyncCallback` don't run) and **carry forward** the real field's
`unk180/191/194` (sun) and `MARIO_FLAG_OCCLUDED`/`gpSilhouetteManager->unk48` (Mario+
marukage), while still **re-issuing all the geometry** (sun disc, lens flare/glow,
specular, Mario silhouette, marukage, water silhouette) so nothing goes missing.

### Unresolved / flagged
- **スペキュラシーン concrete class/source not located** — referenced only by name; no
  `TSpecularSheen` in `src/`. It is data-defined (scene `.bin`), behaves as a pure consumer
  of TSunModel's dispersion ratio (alpha-ramp class, not an EFB reader). If the in-between
  needs special handling beyond "re-issue draw," inspect the live object's class via the
  probe at runtime.
- **TLensGlow::perform exact address** not enumerated in the grep above (it is in the
  `TLensGlow` block of `reference/sms_gmse01_funcs.txt`); minor — `lensglow.cpp` is the
  source and it reads no EFB.
- The marukage's ground consumer is the map/ground geometry that follows TSilhouette in
  `PerformList Silhouette` (texmtx 0x1e left active); the water consumer is confirmed
  (`TModelWaterManager::drawSilhouette` 0x8027dd00). The exact *map* object that renders
  with texgen `GX_TG_POS`/0x1e was not isolated to a single named draw — it is the
  scene-data ground in that perform list. Flagged for runtime confirmation if needed.
