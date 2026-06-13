# Water surface + Mario shadow — per-field determinism for 60fps re-issue

Follow-up to `water_rendering.md` and `sun_specular_efb_effects.md`. Those two docs
established that (a) the water refraction samples an EFB copy (the "screen texture") and
(b) the sun Z-probe (`GXPeekZ`×17) and Mario alpha-probe (`GXPeekARGB`) are EFB-feedback
reads. The user reports that the **water surface** and **Mario's shadow** still flicker
(present/different on alternating fields) AFTER two failed fixes:

- **Failed fix A:** reusing the real field's EFB screen texture → ghosts the reflection.
- **Failed fix B:** suppressing the EFB-feedback draw-sync callbacks (sun `GXPeekZ`,
  Mario `GXPeekARGB`) → still flickers.

So the cause is NOT (only) the peek-based occlusion/sparkle. This doc enumerates every
OTHER per-field-varying input to the water-surface draw and the shadow draw, and pins the
shared cause that survives both fixes.

All addresses are GMSE01 (`reference/sms_gmse01_funcs.txt`); file:line refer to
`reference/sms/`. Classifications: **(i) constant**, **(ii) camera/matrix-derived**
(interpolated, fine), **(iii) EFB-sourced** (varies because the in-between EFB holds the
interpolated scene), **(iv) frame-counter / calc-phase-sourced** (frozen on the in-between
→ juddery if it should move, or desynced if double-advanced).

---

## 1. `TModelWaterManager::drawRefracAndSpec` (0x8027c12c) — every input

Source: `reference/sms/src/Player/ModelWaterManager.cpp:1442`. Called from `perform`'s
`& 0x80` (post) phase (`:1584`). The three sub-passes each gate on a surface flag
(`unk5D60`, a constant `367 = 0x16F`, set at `:148`): refraction (`& 2`, `:1503`),
water-color (`& 4`, `:1521`), specular sparkle (`& 8`, `:1537`).

| # | Input | Source (file:line) | Class | Notes |
|---|---|---|---|---|
| 1 | **Screen texture** `unk5D34` bound `GX_TEXMAP0` | `:1474`; handle cached in `loadAfter` from "スクリーンテクスチャ" (`:194`) | **(iii) EFB** | The refraction layer. THE one per-field-varying texture. See §2. |
| 2 | Indirect bump map `unk5D38` (`waterref`) `GX_TEXMAP1` | `:1475` | (i) const | Static `.bti`. |
| 3 | Mask `unk5D3C` (`waterMask`) `GX_TEXMAP2` | `:1476` | (i) const | Static `.bti`. |
| 4 | Specular `unk5D40` (`waterSpec`) `GX_TEXMAP3` | `:1523` | (i) const | Static `.bti`. |
| 5 | Projection texmtx `0x1e` | `C_MTXLightPerspective(m, gpCamera->getFovy(), gpCamera->getAspect(), …)` `:1457`, loaded `:1459` | **(ii) camera** | Screen-space projection of water verts. Interpolates cleanly with the camera. |
| 6 | TEXCOORD1 texmtx `0x3c` | `GX_TG_TEX0` `:1455` (identity 0x3c) | (i) const | Identity slot. |
| 7 | Indirect warp scale matrix | `unk5D1C` = `0.07f` (`:112`), into `local_84` `:1465-1471` | (i) const | Warp magnitude is fixed. |
| 8 | `GXSetTevColor(GX_TEVREG0, {0,0,0,unk5D65})` | `unk5D65` = `255` (`:153`), `:1477` | (i) const | Refraction-pass alpha. |
| 9 | `GXSetTevColor(GX_TEVREG0, gModelWaterManagerWaterColor[unk5D5F])` | `unk5D5F` = `0` (`:147`), `:1510` | (i) const | Water-color tint; index never changes. |
| 10 | `GXSetTevColor(GX_TEVREG0, unk5D20)` | `unk5D20` = `{0xbc,0xcc,0xdc,0xff}` (`:113`), `:1524` | (i) const | Spec-pass color reg 0. |
| 11 | `GXSetTevColor(GX_TEVREG1, unk5D24)` | `unk5D24` = `{0x8e,0x8e,0x9e,0xff}` (`:114`), `:1525` | (i) const | Spec-pass color reg 1. |
| 12 | TEV stage config / blend / Z / fog | `:1478-1502, 1506-1536` | (i) const | All literal GX setup; no per-field data. |
| 13 | Water quad geometry `unk5D30->draw()` | `:1504, 1522, 1538`; vtx built in `calcDrawVtx`/`calcVMAll` under `& 4` (`:1555-1556`) | (ii) camera | Vertices rebuilt from `param_2->mViewMtx` in the `& 4` calc phase. |

**No `J3DFrameCtrl` scroll counter** is read in `drawRefracAndSpec` — the spec sparkle has
no independent animation phase (confirmed in `water_rendering.md` §3.2). **No `gpSunMgr`
read**: `drawRefracAndSpec` does NOT touch `gpSunMgr` / `getUnk1CAlpha` / `unk18` / the sun
color at all (grep-verified; the only `gpSunMgr` consumers are `lensflare.cpp`,
`DrawUtil.cpp`, and the draw-sync registration). So the prompt's hypothesis that the water
*surface* reads the sun color is **false for `drawRefracAndSpec`** — every TEV register
color it sets is a load-time constant (rows 8–11).

**Conclusion for the surface:** of all 13 inputs, exactly **one is per-field-varying — the
screen texture (row 1, EFB)**. Everything else is constant or cleanly camera-interpolated.
The water surface's only flicker source is the refraction layer.

---

## 2. The refraction = a warped re-projection of the EFB (confirmed mechanism)

`drawRefracAndSpec`'s stage-0 samples `GX_TEXMAP0` = the screen texture, at TEXCOORD0
(world-pos → screen via the camera-projection texmtx, row 5), **displaced** by the
indirect `waterref` lookup (`GXSetTevIndWarp` `:1463`, warp scale 0.07). So each water
pixel = "the EFB pixel roughly under this water vertex, jittered by the bump map."

The screen texture is `GXCopyTex` of the EFB taken by the "通常シーン描画ステージ"
`TEfbCtrlTex` in the GX list's copy phase (`0x802f8bac`, copy bit `& 0x8`), AFTER the world
is in the EFB — full detail in `water_rendering.md` §2a–2b.

**On the in-between field the EFB contains the INTERPOLATED scene** (Mario/NPCs at midpoint
poses, the camera at the midpoint). Two scenarios, both broken:

- **Re-render world + re-copy EFB for the in-between (the "correct-looking" path):** the
  refraction reflects the interpolated scene. This is self-consistent, but the *content*
  reflected genuinely differs from the real field's refraction every other field. If the
  interpolated scene differs visibly from the real scene at any reflected pixel (and it
  does — that is the whole point of interpolating), the refraction layer alternates between
  two slightly different images → the surface "shimmers"/flickers at 30 Hz beat.
- **Reuse the real field's screen texture (failed fix A):** the surface geometry/warp/
  projection are at the in-between (interpolated) state, but the reflected content is the
  real field's → moving objects in the reflection lag → ghost/double-image.

**Is the refraction strong enough to BE the flicker?** Yes. The refraction pass is the
FIRST and dominant water sub-pass: stage-0 outputs the screen-texture color directly into
`GX_TEVPREV` (`GX_CC_TEXC`, scale 1, `:1480-1483`), masked by `waterMask`. The water-color
tint (pass 2, row 9) and the additive specular (pass 3, blend `GX_BL_ONE` `:1535`) layer ON
TOP of it. So the bulk of the visible water color **is** the refracted EFB. Any per-field
difference in the EFB shows directly in the water. This is consistent with the user seeing
the water surface itself flicker, not just the sparkle.

**Can the water be drawn WITHOUT sampling the live EFB?** Not faithfully. The refraction is
not optional in the data: `unk5D60 = 367 = 0x16F` has bit 2 set, so the `& 2` refraction
pass always runs (`:1503`). There is no flag to substitute a fixed source — the screen
texture handle `unk5D34` is the single bound input and it is whatever the most recent
`TEfbCtrlTex` copy produced. The only way to make the in-between refraction *identical* to
the real field is to feed it the **same EFB copy** as the real field — which is exactly
failed fix A and it ghosts (because the surface moved but the reflection didn't).

---

## 3. The shadow (marukage + silhouette) — color/alpha source and gating

Two cooperating shadow draws, both keyed to texmtx `0x1e` built from `gpMarioPos`:

### 3a. The marukage projector — `TSilhouette::perform & 0x10` (DrawUtil.cpp:116, addr 0x80227914)
Builds the projective texmtx `0x1e` from `gpMarioPos` (`C_MTXLightFrustum` + rot/scale/
trans by `-gpMarioPos->x/z`, `:117-133`) and sets up a 2-stage TEV (`:149-165`) that
modulates `H_marukage_xlu_i8.bti` (TEXMAP1) onto the ground/water that follows. Its color:

| Input | Source (file:line) | Class |
|---|---|---|
| `GXSetChanMatColor(GX_COLOR0A0, unk12)` | `unk12` (`:145-146`) | see below |
| `GXSetTevColor(GX_TEVREG0, {…, a=0x40})` | `unk12` with alpha forced `0x40` (`:147-148`) | RGB from `unk12`, alpha const |
| texmtx `0x1e` projector | `gpMarioPos` (`:127`) | **(iv) calc** — `gpMarioPos` updates only at the 30 Hz tick |
| `gpMarioPos` matrix | `gpMarioPos` | (iv) calc, frozen on in-between (half-frame lag, not flicker) |

`unk12` is the shadow/silhouette tint. Origin chain:
- `unk12 = gpSunMgr->unk18` in `loadAfter` (`DrawUtil.cpp:35`), then `unk12.a = 0`.
- `gpSunMgr->unk18` is a **load-time constant** read from the scene stream (`sunmgr.cpp:48`,
  `unk18.set(col1)`); `TSunMgr::perform` does NOT update it (its `& 1` body is only a
  noki-bay warp check, `sunmgr.cpp:73-96`). So the shadow RGB is **(i) constant**.
- **BUT `unk12.a` is overwritten every calc tick:** `TSilhouette::perform & 1` does
  `unk48 += unk4C*(occluded?128:0 - unk48); unk12.a = unk48;` (`:96-98`). The marukage TEV
  alpha (`:147`) hardcodes `0x40`, so the marukage's OWN alpha is constant; but the marukage
  spotlight RAS color comes through the `setting()` light whose alpha = `unk48` (see 3b).

### 3b. The water silhouette pass — `TModelWaterManager::drawSilhouette` (0x8027dd00, ModelWaterManager.cpp:920)
Runs in `perform & 8` (`:1566`), gated at the top by
`if (!gpSilhouetteManager->isUnk48Positive()) return;` (`:922`) — i.e. **only drawn when
`gpSilhouetteManager->unk48 > 0`**. Its shadow color/alpha:

| Input | Source (file:line) | Class |
|---|---|---|
| Soft-shadow cube alpha | `unk5D5D * gpSilhouetteManager->unk48 * 0.00390625f` (`:981`) | **(iv) calc** — `unk48` from the occlusion low-pass |
| Final cube color | `gpSilhouetteManager->unk12` with alpha `unk5D5D * unk12.a` (`:989-992`) | RGB const (sun color), **alpha = `unk12.a = unk48`** (iv) calc |
| `unk5D5D` | `80` (`:145`) | (i) const |

So **both** the water silhouette alpha (`:981`) and the final-cube alpha (`:992`, via
`unk12.a`) are driven by `gpSilhouetteManager->unk48` — the smoothed Mario-occlusion alpha,
updated ONLY in `TSilhouette::perform & 1` (the 30 Hz calc tick, `DrawUtil.cpp:97`).

### 3c. The ground-darken spotlight — `TSilhouette::perform & 8 / & 0x80` (DrawUtil.cpp:101-115)
`setting()` (`:68`) installs a spot light at `gpMarioPos` whose color alpha = `unk48`
(`:78-80`, `unk16.a = unk48`). The `& 0x80` variant overrides the *material* color alpha
with `gpSunMgr->getUnk1CAlpha()` (`:112`) — but `unk1C` is, like `unk18`, a **load-time
constant** (`sunmgr.cpp:49`). So the ground-darken intensity is `unk48`-driven (calc).

**Net for the shadow:** its RGB tint is a constant (sun color from the scene file, NOT
updated per-frame). Its **strength/alpha is entirely `gpSilhouetteManager->unk48`** —
the occlusion low-pass — across the marukage spotlight, the water silhouette, and the
ground-darken. `unk48` is updated only in `& 1` (calc). The whole water-silhouette draw is
*gated on `unk48 > 0`* (`:922`), and the whole `mPerformListSilhouette` is gated on
`unk48 > 0` in the director (`MarDirectorDirect.cpp:172`, per `sun_specular_efb_effects.md`
§4).

---

## 4. `gpSunMgr` (TSunMgr) — what is calc-updated vs constant

This is where the prompt's framing needs correcting. There are **two distinct objects**:

- **`gpSunMgr` (TSunMgr, `sunmgr.cpp`)** — owns `unk18` and `unk1C` colors. Both are set
  ONCE in `load` from the scene stream (`:46-49`) and **never updated** (`perform` is a
  warp check only). So `getUnk1CAlpha()`, `unk18`, the shadow RGB — all **(i) constant**.
  `gpSunMgr->drawSyncCallback` merely forwards to `gpSunModel->getZBufValue()` (`:108-109`).
- **`gpSunModel` (TSunModel, `sunmodel.cpp`)** — the per-field EFB readback object (the
  `GXPeekZ`×17, `sun_specular_efb_effects.md` §1a). Its `unkAC` (additive glow,
  `:222-226`), `unk191/unk194` (dispersion) ARE updated per readback. These feed the **sun
  disc / lens flare / glow**, NOT the water surface and NOT the shadow color.

So: **the sun color does NOT couple the water and the shadow.** `drawRefracAndSpec` reads
no sun color (§1), and the shadow's sun-derived RGB is a frozen constant (§3). The water
specular and the shadow are NOT synchronized through `gpSunMgr`. They are synchronized
through something else (§5).

---

## 5. CONCLUSION — the shared cause that survives both failed fixes

The water and shadow share **one** per-field-varying input that neither failed fix
addressed: **the EFB itself.** Specifically:

1. **Water surface flicker = the refraction layer (the screen texture / EFB copy).** This
   is the only per-field-varying input to `drawRefracAndSpec` (§1, §2). It is NOT a peek and
   NOT a callback, so suppressing the draw-sync callbacks (failed fix B) does nothing to it.
   It is NOT a frozen counter, so it does not judder — it genuinely alternates between the
   real-field EFB content and the interpolated-field EFB content every other field. Reusing
   the real EFB copy (failed fix A) fixes the flicker but ghosts moving objects, because the
   surface projection (camera texmtx, row 5) IS interpolated while the reflected content is
   not → the two disagree.

2. **Shadow flicker = `gpSilhouetteManager->unk48` + the `unk48 > 0` gate, coupled to the
   EFB through the occlusion probe.** The shadow's strength is entirely `unk48` (§3), the
   smoothed Mario-occlusion alpha. `unk48` is updated only in the calc tick (`& 1`), so on a
   pure draw-only in-between it is *frozen* — which would give a half-frame of lag, not
   flicker. The prompt notes `unk48` is now frozen (EFB-feedback suppressed), so the residual
   shadow flicker is NOT `unk48` toggling.

   The residual shadow flicker is therefore the **marukage/silhouette geometry being
   composited over a different EFB on alternating fields**, AND/OR the `unk48 > 0` gate
   toggling the whole `mPerformListSilhouette` (and `drawSilhouette`'s early-return at
   `:922`) on/off when `unk48` sits near zero. If `unk48` is held frozen, the gate stops
   toggling — but the marukage texture is projected onto the ground/water surface, and where
   that ground IS the water (the water silhouette pass `:920`), the shadow is drawn into the
   same EFB-sampled water that is already flickering. **The shadow rides on the water.**

**Why they flicker IN SYNC:** not through the sun color (§4 rules that out), but because
both are composited against / into the **same EFB whose contents alternate** between the
real-field render and the interpolated-field render. The water samples that EFB (refraction);
the shadow is drawn onto the water (which is that EFB). One root: **the in-between field's
EFB differs from the real field's EFB, and both the water refraction and the water-projected
shadow are functions of that EFB.**

### Is a correct fix possible?

The fundamental tension: the refraction wants the EFB to MATCH the surface it's drawn on
(self-consistency, no ghost), but the EFB content also IS the thing the user perceives as
flicker when it alternates. Two viable directions:

- **(A) Draw the EFB-refraction layer once, at 30 fps.** Render the water refraction pass
  (`& 2`, the dominant layer) only on real fields; on the in-between, re-issue only the
  camera-interpolated, EFB-independent layers (water-color tint `& 4`, specular `& 8`) and
  the geometry, reusing the real field's refraction RESULT as a static layer keyed to the
  real field's projection. This removes the alternation (no flicker) at the cost of the
  refraction not interpolating — acceptable because the refraction is low-frequency content
  (warped, half-res RGB565) where a 30 Hz update is hard to notice, unlike the surface
  geometry which stays 60 Hz. The shadow rides along: if the water layer it's composited
  onto is stable, the shadow stops flickering too.

- **(B) Recompute the in-between refraction from the SAME EFB as the real field, but
  re-projected through the in-between camera.** I.e. take the real field's screen-texture
  copy (no re-render, no ghost from re-rendering) but re-run `drawRefracAndSpec` with the
  in-between camera's `C_MTXLightPerspective` (row 5) so the projection matches the
  in-between surface. This is failed fix A *plus* re-projecting the lookup — the ghost in
  fix A came from the projection NOT being updated. Whether the residual parallax/ghost is
  acceptable depends on how much the camera moved in a half-frame; for a slow plaza camera
  it should be small. This is the more faithful option (keeps 60 Hz refraction motion) but
  risks a subtle ghost on fast camera motion.

**Recommended:** start with **(A)** — render the refraction layer at 30 fps and re-issue
only the EFB-independent water layers + shadow geometry at 60 fps. It is the only option
that eliminates the alternation entirely (the alternation IS the EFB difference, and (A)
removes the EFB from the in-between path), and it simultaneously fixes the shadow because
the shadow is composited onto the now-stable water. (B) is the fallback if 30 Hz refraction
motion reads as visibly choppy.

### Flagged / uncertain

- The exact perform-phase masks are data-driven (`PerformLists.bin`); the `& 2/4/8` sub-pass
  gating via `unk5D60 = 367` is from source and is constant, but confirm at runtime that the
  refraction sub-pass (`& 2`) is the dominant visible layer in the plaza specifically (it is
  by TEV construction, §2, but a runtime EFB capture would settle it).
- Option (A) assumes the half-res RGB565 refraction is low-frequency enough that a 30 Hz
  update is imperceptible. This is a perceptual claim — verify headed after implementing.
- `gModelWaterManagerWaterColor[unk5D5F]` is indexed by `unk5D5F`, only ever `0` in the
  decomp (`:147`); if a stage script rewrites it the tint could change per scene, but never
  per field. Not a flicker source.
