# Marukage (Mario round drop-shadow) per-field flicker — RE + fix

Target: the **marukage** — Mario's soft round drop-shadow blob projected onto the GROUND
and onto the WATER surface. Under the 60fps in-between-field re-issue (`SUNBRIGHT_INTERP60`)
it **flickers on the in-between field**, on ground and water in sync. This is NOT the water
refraction/reflection (`drawRefracAndSpec`) — that is a separate effect and is not the
subject here.

All file:line refer to the vendored decomp `reference/sms/`; addresses are GMSE01
(`reference/sms_gmse01_funcs.txt`). The renderer facts are verified against our actual
renderer, Dolphin's `VertexShaderGen.cpp` (we own that fork), not from memory.

---

## 0. TL;DR conclusion (read this first)

**The crux question — what space does `GX_TG_POS` feed the texgen — answered rigorously:
`GX_TG_POS` feeds the texgen the RAW, UNTRANSFORMED input vertex position (object space),
NOT the post-position-matrix (eye/view) space.** The geometry's position matrix (PNMTX,
the model-view) is applied only to produce the clip-space output position `o.pos`; it does
**not** touch the texgen path. The texgen coordinate is `coord = rawpos.xyz`, transformed
by the texgen matrix (texmtx `0x1e`) alone.

Verified in `externals/dolphin/Source/Core/VideoCommon/VertexShaderGen.cpp`:
- `SourceRow::Geom` (the row `GX_TG_POS` selects, see `J3DGDSetTexCoordGen` row=0 below) →
  `coord.xyz = rawpos.xyz;` (line 643). `rawpos` is the **raw vertex**, before any matrix.
- `dolphin_transform_texcoord(coord)` → `result = TEXMATRICES[slot] · coord` (lines 195-203)
  — the texgen matrix is applied to that raw coord directly.
- The model-view PNMTX is applied separately inside `process_vertex` /
  `dolphin_process_emulated_vertex` to `vertex_input.position = rawpos` (line 605) to make
  `o.pos` via the projection matrix (line 700). **Two independent paths; the texgen never
  sees the model-view-transformed (eye-space) position.**

Consequence: the marukage texmtx `0x1e` (built by `TSilhouette::perform &0x10`) contains
**no camera / no view matrix** (`C_MTXLightFrustum` is a pure projection, `mtx.c:503`; the
rest is rot/scale/translate-by-`gpMarioPos`). The shadow's UV on the receiving surface is a
function of the surface's **object/world position and `gpMarioPos` only — it is
camera-independent by construction.** On the real field it lands correctly because the map's
node matrix places map vertices at world coordinates (object≈world for the static map), so
`rawpos` ≈ world position, and the texmtx translates by world `-gpMarioPos->x/z`.

**Why it flickers on the in-between field:** the 60fps path
(`runtime/overrides/interp_capture.cpp`) interpolates each model's **per-joint draw matrix**
`drawMtx = viewMtx × nodeMatrix` in `mDrawMtxBuf[1][view]` toward the in-between (the
interpolated camera). So the ground geometry's **clip position `o.pos` moves to the
interpolated-camera projection**, but its **texgen input (`rawpos` → texmtx `0x1e`) does
NOT move** — the texmtx is camera-independent and `gpMarioPos` is frozen at 30Hz. The
shadow UV is therefore pinned to where it would be under the REAL field's camera, while the
ground pixels it modulates are drawn under the INTERPOLATED camera. The shadow texture and
the surface it is stamped on are projected with **two different cameras** → the shadow blob
shifts relative to the ground between the real and in-between field → **per-field flicker /
swim.** It is a *projection-mismatch* flicker, not an on/off blink (the instrument already
confirmed `perform&0x10` fires equally on both fields).

**The fix (one line of principle):** the marukage texgen must be re-projected with the
**same (interpolated) camera the receiving geometry uses on that field.** Since the texmtx
`0x1e` carries no camera, the actual offending term is the **receiving ground's draw matrix
being interpolated while the texgen stays still**. Two equivalent remedies, detailed in §5:
either (A) **rebuild texmtx `0x1e` for the in-between field so the shadow projection is
consistent with the interpolated ground draw matrix** (the correct, general fix), or (B)
**do NOT interpolate the draw matrix of the marukage-receiving surfaces** (ground/map +
water disc), so the camera that draws them matches the (frozen) texgen — cheaper, but it
de-interpolates the ground.

**Why interpolating `gpMarioPos` made it WORSE:** `gpMarioPos` moves the texmtx (the shadow
CENTER) independently of everything else. The shadow center must stay locked to the surface
it is drawn on; nudging only the center while the surface and the rest of the projection do
not follow it adds a *second*, larger desync on top of the camera mismatch. The texmtx is
not an independent knob — it is one half of a projection that must match the receiving
geometry's transform.

---

## 1. The crux: what coordinate space does `GX_TG_POS` actually feed? (verified)

`TSilhouette::perform &0x10` sets:
```
GXSetTexCoordGen2(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_POS, 0x1e, 0, 0x7d);   // DrawUtil.cpp:137
```
- `GX_TG_POS` selects XF input **row 0 = position** (`J3DGDSetTexCoordGen`,
  `JRenderer.cpp:79-82`: `case GX_TG_POS: row = 0; form = 1;`).
- `0x1e` is the texgen matrix slot (texmtx, loaded by `GXLoadTexMtxImm(afStack_50, 0x1e,
  GX_MTX3x4)`, `DrawUtil.cpp:133`).
- `0x7d` is the post-transform matrix = `GX_PTIDENTITY` (dual-tex post-matrix = identity).

**What "position" means at row 0 — the rigorous answer (Dolphin shader, our renderer):**
the input fed to the texgen is the **raw vertex position** (`rawpos`, object space), and the
texgen matrix is applied to it directly. It is *not* the eye-space position produced by the
geometry's PNMTX. Evidence chain (`VertexShaderGen.cpp`):

| step | code | meaning |
|---|---|---|
| texgen input | `case SourceRow::Geom: coord.xyz = rawpos.xyz;` (line 643) | raw object-space vertex |
| texgen transform | `result = TEXMATRICES[3*i..] · coord` (lines 195-203) | texmtx `0x1e` × raw pos |
| separate position path | `vertex_input.position = rawpos;` (605) → `process_vertex` → `o.pos = PROJECTION · vertex_output.position` (700) | PNMTX/model-view affects ONLY `o.pos`, not the texgen |

So: **`GX_TG_POS` is object/pre-position-matrix space.** The texgen is camera-independent;
only the on-screen *placement* of the pixels (`o.pos`) is camera-dependent. (This matches GC
hardware: the XF texgen INROW taps the geometry input coordinate, and the texgen/dual-tex
matrices do their own transform — the position matrix and the texgen matrix are distinct XF
stages.)

**How the receiving ground is drawn (what PNMTX is active):** J3D loads the shape's position
matrix as `drawMtx = viewMtx × nodeMatrix`:
- `J3DModel::viewCalc` computes `J3DMTXConcatArrayIndexedSrc(viewMtx, mNodeMatrices, …)` into
  `mDrawMtxBuf[1][view]` (`J3DModel.cpp:794-797`) — `viewMtx` = camera, `nodeMatrix` = world
  joint placement.
- `J3DShapeMtx::loadMtxIndx_*` loads that into the PNMTX via `GXLoadPosMtxIndx`
  (`J3DShape.cpp:17-39`, `J3DSys::loadPosMtxIndx` :53).

So the ground's PNMTX **is eye-space (includes the camera)**, but — per the crux above — that
PNMTX feeds `o.pos` only. The shadow texgen is fed the ground's **object/world** `rawpos`
through texmtx `0x1e`. **The shadow is therefore camera-independent; the ground pixel
positions are camera-dependent.** That split is the entire bug.

---

## 2. Per-field inputs to the marukage render — consistent or differing across the two fields?

The marukage state is set up by `TSilhouette::perform &0x10` (`DrawUtil.cpp:116-166`); the
shadow is then *consumed* by the ground/map draw (texmtx `0x1e` left active) and by the water
disc (`TModelWaterManager::drawMirror`, `ModelWaterManager.cpp:1161`, uses texgen `GX_TG_POS`
/ slot `0x1e` at :1230). Classification for the in-between field (which re-issues the GX draw
list but does NOT re-run the 30Hz `&1` calc tick):

| input | source | per-field on the in-between? |
|---|---|---|
| **texmtx `0x1e`** (the projector) | `C_MTXLightFrustum` (no view) × rot `0x58` × `PSMTXScale(unk3C)` × `PSMTXTrans(-gpMarioPos->x,0,-gpMarioPos->z)` × `PSMTXTrans(1.75,1.75,0)` (`DrawUtil.cpp:117-133`) | **IDENTICAL on both fields** (built from frozen 30Hz `gpMarioPos`; no camera). This is exactly the problem — it is camera-independent, so it does NOT follow the interpolated ground. |
| **receiving ground draw matrix** (PNMTX = `viewMtx × nodeMatrix`) | `J3DModel::viewCalc` → `mDrawMtxBuf[1][view]`; **interp60 blends this** (`interp_capture.cpp:61-114`, `blend_model`) | **DIFFERS** — the in-between uses the interpolated camera. This is the per-field-varying term that the texgen fails to match. |
| **camera / view** | `gpCamera` / `viewMtx` threaded into the draw | **DIFFERS** (interpolated for the in-between) — affects `o.pos`, NOT the texgen. |
| **`unk3C` scale** | `TSilhouette` field, set at 30Hz | identical (frozen). |
| **`gpMarioPos`** | `&gpMarioOriginal->mPosition`, 30Hz | identical (frozen) unless we deliberately interpolate it (which made it worse, §3). |
| **TEV setup** (2-stage modulate, `DrawUtil.cpp:149-160`) | static GX state | identical. |
| **`GXSetZMode(GX_TRUE, GX_GEQUAL, GX_FALSE)` + `GXSetZCompLoc(GX_TRUE)`** (`DrawUtil.cpp:164-165`) | static GX state | identical — but note the **depth test is against the interpolated-camera Z** of the ground, so where the GEQUAL test passes can shift slightly per field (secondary, minor vs the projection swim). |
| **alpha / `gpSilhouetteManager->unk48`** | low-passed at 30Hz (`DrawUtil.cpp:95-99`) | identical (frozen across the in-between). |

**Net: exactly one input differs in a way that breaks the projection — the receiving
surface's draw matrix is interpolated (camera-moved) while the shadow's texgen (texmtx
`0x1e`) is frozen camera-independent.** Everything else is consistent. So the flicker is the
**relative motion between the camera-interpolated ground pixels and the camera-frozen shadow
UV**.

---

## 3. Why interpolating `gpMarioPos` made it WORSE (the strong clue, explained)

`gpMarioPos` enters only the texmtx `0x1e` translation `PSMTXTrans(-gpMarioPos->x, 0,
-gpMarioPos->z)` (`DrawUtil.cpp:127`) — it sets the shadow's **center** in world space. The
texgen output for a ground vertex is `texmtx0x1e · rawpos_world`. The shadow lands correctly
when the texmtx is the one consistent half of a projection whose other half is the receiving
geometry's transform.

Interpolating `gpMarioPos` moves the shadow center **independently** of (a) the receiving
ground's interpolated camera and (b) the rest of the projection. So instead of fixing the
camera mismatch (the real defect), it injects a *second* desync: the blob center now slides
by a half-tick of Mario motion on top of the existing camera swim. Because Mario's per-tick
translation is typically much larger than the sub-pixel camera delta of a 16 ms in-between,
the added center slide dominates → visibly worse.

The lesson the clue carries: **the texmtx is not an independent center knob.** The shadow
center, the projector, and the receiving geometry's transform are one coupled projection.
Move one, you must move all consistently — or move none.

---

## 4. The shadow on WATER — same source, in sync (confirmed)

The marukage reaches the water surface through the **same texmtx `0x1e`** built by
`TSilhouette::perform &0x10`, consumed by `TModelWaterManager::drawMirror`
(`ModelWaterManager.cpp:1161`):
- `drawMirror` loads the **view matrix** into PNMTX0 (`GXLoadPosMtxImm(param_1, …)` :1225,
  `param_1` = the field's `mViewMtx`) and draws a triangle fan with **world-space vertices**
  (`SMS_GetMarioPos().x/y/z` and a 1000-unit ring, :1198-1213, :1240-1250).
- texgen: `GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3x4, GX_TG_POS, 0x1e, …)` (:1230) —
  **identical mechanism**: raw (world) vertex through texmtx `0x1e`.
- It does **not** rebuild `0x1e`; it reuses the slot left active by `TSilhouette`. (NOTE: the
  prior doc `sun_specular_efb_effects.md` §3 said the water manager "rebuilds the same
  `GXLoadTexMtxImm(…,0x1e,…)`" at 1230/1454-1459 — that is imprecise. Line 1230 only *uses*
  `0x1e`; the `GXLoadTexMtxImm(…,0x1e,…)` at :1459 belongs to `drawRefracAndSpec`, a
  DIFFERENT effect (water refraction, `C_MTXLightPerspective` of the live camera, identity
  PNMTX) — not the marukage. Corrected here.)

So the water disc has the **same camera-independent texgen** and the same
interpolated-camera PNMTX for `o.pos`. Its flicker is therefore the **same root cause** as
the ground, which is exactly why the user sees ground and water flicker **in sync** — they
share texmtx `0x1e` and both have it frozen against an interpolated camera.

(`drawMirror`'s vertices are world-space and so are also *not* interpolated per-field — the
fan is rebuilt from frozen `SMS_GetMarioPos()`; but its `o.pos` IS camera-interpolated via
`param_1`=interpolated view. Same split as the ground.)

---

## 5. CONCLUSION — mechanism + concrete fix

### Mechanism (exact)
`GX_TG_POS` feeds the texgen the **raw object/world vertex position, not eye space** (proven
in `VertexShaderGen.cpp`: `coord = rawpos`, texmtx applied to it; the model-view PNMTX
affects only `o.pos`). The marukage texmtx `0x1e` is built **without any camera**
(`C_MTXLightFrustum` + Mario-position translate, `DrawUtil.cpp:117-133`), so the shadow's UV
on the receiving surface is **camera-independent and frozen at 30Hz**. On the in-between
field, interp60 blends the **receiving surface's draw matrix** (`viewMtx × nodeMatrix`,
`interp_capture.cpp` `blend_model`) toward the **interpolated camera**, moving the ground/
water pixels on screen — but the shadow texgen does not move with them. The shadow is thus
projected with the real-field camera while the surface is drawn with the in-between camera →
the blob swims relative to the ground/water every other field → the observed per-field
flicker, in sync on ground and water (shared texmtx `0x1e`).

### The fix
The shadow projection must use the **same camera as the receiving geometry on each field.**
Concretely, on the in-between field:

- **(A) Preferred — re-project the texgen for the in-between camera.** The shadow UV must be
  consistent with the interpolated draw matrix the ground/water use. Because the SMS texmtx
  `0x1e` is built camera-free *and the texgen tap is object/world `rawpos`*, the shadow is
  meant to be camera-independent by design — meaning **the correct invariant is that the
  receiving surface's screen projection and the shadow projection share one camera.** The
  clean implementation is: when interp60 substitutes interpolated draw matrices for the
  in-between, ensure the shadow's *receiving* surfaces are projected with the same camera
  that the shadow texgen implies. Since the shadow texgen is camera-free (world-anchored),
  that means the marukage and its receiving surfaces are only consistent when projected with
  a *single, matching* camera. Practically: **interpolate consistently or not at all** — see
  (B) for the simplest correct form.

- **(B) Simplest correct fix — do not interpolate the marukage-receiving surfaces' draw
  matrices on the in-between field.** Because the shadow texgen is world-anchored and frozen,
  the surfaces it modulates (the map/ground model and the water disc/`drawMirror` fan) must
  be drawn with the **same camera the texgen was built against = the real field's camera**,
  i.e. NOT camera-interpolated for the in-between. In `interp_capture.cpp`'s `blend_model`,
  **exclude the map/ground model (and the water-disc draw) from draw-matrix interpolation**
  so their `o.pos` uses the real-field (frozen) camera, matching the frozen shadow texgen.
  The shadow then sits rock-still relative to the ground/water across both fields. Cost: the
  ground stops being interpolated (it snaps at 30Hz) — acceptable for a near-static plaza
  floor; if the camera is what moves, the whole ground would judder, so prefer (A)/(C) when
  the camera pans.

- **(C) Most faithful to 60fps feel — rebuild texmtx `0x1e` (and `drawMirror`'s vertices) for
  the in-between with the matching interpolated state.** Keep interpolating the ground draw
  matrix (camera), AND on the in-between re-run `TSilhouette::perform &0x10`'s texmtx build
  so the shadow projection is recomputed for the *same* field. Since texmtx `0x1e` is
  camera-free, "matching" here means recomputing it from the **same `gpMarioPos` the ground
  draw assumes for that field** — and crucially **keeping `gpMarioPos` consistent between the
  texmtx and any Mario-model interpolation** (do not move one without the other; that is the
  §3 lesson). In practice: if Mario's model IS interpolated on the in-between, interpolate
  `gpMarioPos` AND rebuild texmtx `0x1e` from the interpolated value in the same pass; if
  Mario is NOT interpolated, leave both frozen. Never interpolate `gpMarioPos` alone
  (proven worse, §3).

### Do NOT
- Do **not** interpolate `gpMarioPos` in isolation (§3 — strictly worse).
- Do **not** add a magic per-field UV offset to "line up" the shadow — that is a bandaid and
  will drift with camera speed.

### Recommended concrete step
Start with **(B)**: in `runtime/overrides/interp_capture.cpp` `blend_model`, skip
draw-matrix interpolation for the marukage-receiving surfaces (the map/ground model; the
water silhouette/mirror draw is world-vertex so already un-interpolated for vertices but its
PNMTX=view is interpolated — pin that to the real-field view too). Verify with the
`/interp60` present-ring + a headed A/B: the shadow should stop swimming on the in-between
field, in sync on ground and water. If the resulting 30Hz ground judder is objectionable
during camera pans, escalate to **(C)** (rebuild `0x1e` per field with consistent
`gpMarioPos`).

---

## 6. Flagged / uncertain
- **GX_TG_POS coordinate space: VERIFIED, not assumed.** Confirmed object/raw-position (not
  eye-space) directly in our renderer's shader generator (`VertexShaderGen.cpp:643` +
  :195-203 + :605/:700). The prior `sun_specular_efb_effects.md` hypothesis-by-omission is
  superseded by this verification. Confidence: high. (Caveat: this is what *our* renderer —
  the Dolphin fork — emits; it matches GC HW behavior, but the authority cited is our actual
  code path.)
- **Which exact map object renders with texmtx `0x1e`/`GX_TG_POS` as the ground receiver** is
  the scene-data ground geometry in the silhouette perform list (texmtx left active by
  `TSilhouette`); not isolated to a single named class in source. For fix (B) the practical
  selector is "the model(s) drawn between `TSilhouette::perform &0x10` and the next texgen
  reset" — confirm live via the GX-stream/interp60 instrument if the exclusion needs to be
  surgical.
- **`drawMirror` PNMTX vs vertices**: its fan vertices are world-space (frozen), its PNMTX is
  the interpolated view — so pinning its view to the real field (fix B) is the matching move
  there.
- Secondary `GEQUAL`/`ZCompLoc` depth-test shift across fields (the ground Z is
  camera-interpolated) is real but minor relative to the projection swim; addressing the
  camera mismatch (B/C) also resolves it since the receiving surface and shadow then share a
  camera.
