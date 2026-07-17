# RE: 60 fps interpolation of screen-space / EFB-feedback effects (SMS)

How to present SMS's EFB-feedback effects (the plaza water refraction above all) correctly
when we synthesize an in-between field at 60 fps. Grounded in the vendored decomp at
`decomp/sms/` and the existing notes:
- `docs/re_notes/vi_xfb_present.md` — single XFB, the two-present alt-address cadence.
- `docs/re_notes/water_rendering.md` — the screen-texture refraction mechanism.
- `docs/re_notes/perform_list_architecture.md` — which effect draws in which list/phase.
- `docs/re_notes/j3d_draw_pipeline.md` — the draw-matrix double buffer the geometry blend edits.

Current implementation: `runtime/overrides/interp_redraw.cpp` (re-issues the draw lists on
the in-between, presents to `orig_fb ^ 0x00400000`) and `runtime/overrides/interp_capture.cpp`
(blends `mDrawMtxBuf` per model). GEOMETRY interpolates correctly. The open problem is the
screen-space layer.

---

## 0. TL;DR / RECOMMENDATION (read this first)

**Recommend strategy (b): render the screen-space / EFB-feedback layer at 30 fps only (on
real fields), and on the in-between field carry forward the REAL field's *composited* water
region — not the source screen-texture.** Concretely: do NOT re-issue the post passes
(`mPerformListGXPost` water `drawRefracAndSpec`, the EFB→screen-texture copy, the mirror /
graffiti EFB passes) on the in-between. Instead, on the in-between field, after the engine has
drawn the interpolated opaque/translucent geometry into the EFB, **composite the real field's
already-finished water/effect pixels over it** by re-presenting the real field's water layer
as a screen-aligned blit. The water surface then updates at 30 fps (no per-field-different
refraction = no flicker) while everything behind it (geometry, camera) moves at 60 fps.

Why not the others, in one line each:
- **(a) re-render+re-capture everything per field** — this is what we do; it flickers because
  the screen-texture content *legitimately differs* between the real and interpolated EFB, and
  the interpolated EFB is **not a correct intermediate** (it interpolated draw matrices in eye
  space, but the refraction's screen-space projection + indirect warp + occlusion peeks are
  non-linear in that), so the reflected image jitters. The flicker is a real artifact of
  interpolating a screen-space readback, not a tuning problem.
- **(c) interpolate the screen-texture / water result between the two real fields** — you do
  not have field N+1 yet at the time you must present the in-between (the in-between is shown
  *before* the next real field is simulated; see §3). True temporal interpolation of the water
  result is impossible without a one-frame present-latency budget the engine's pacing
  (`endRendering` waits, §vi_xfb_present.md) does not give us.
- **(d) reproject opaque+water as a 2D unit** — the water is a *transparent screen-space
  refraction layered over* the opaque scene, not an opaque layer; a single 2D reprojection of
  the composited frame cannot move the geometry under a static refraction, which is the whole
  point of 60 fps. It also reintroduces (a)'s problem for the parallax between the surface and
  what it refracts. Rejected.

The honest tradeoff of (b): **the water surface (and mirror, graffiti readback, dash blur)
animates at 30 fps inside a 60 fps scene.** That is exactly what a real GameCube shows — the
game runs these at 30 fps natively — so it is faithful, and crucially it is *stable* (no
flicker, no ghost). Geometry, camera, Mario, NPCs all move at 60 fps. This is the correct
PC-port choice: 60 fps where it's cheap and correct, native 30 fps where the effect is an EFB
readback that cannot be honestly interpolated.

---

## 1. Catalog: SMS's EFB-feedback / screen-space passes

Every consumer of the EFB-as-input found in the decomp, with its mechanism, where it runs, and
its cost to redo per field. The unifying primitive is `JDrama::TEfbCtrlTex::perform`
(`0x802f8bac`, `JDREfbCtrl.cpp:80`) which, in its **copy phase** (`param_1 & 0x8`), does
`GXSetTexCopySrc` → `GXSetTexCopyDst(half-res)` → `GXCopyTex(mImagePtr)` — EFB → a texture; and
in its **post phase** (`param_1 & 0x80`) issues the pixel-format setting. Plus two direct
EFB-peek state probes.

| # | Pass | Mechanism | Where (list / phase) | Reads EFB? | Writes screen? | Side-effect on game state? | Cost to redo/field |
|---|------|-----------|----------------------|-----------|----------------|----------------------------|--------------------|
| 1 | **Plaza water refraction + specular** (`TModelWaterManager::drawRefracAndSpec` `0x8027c12c`, `ModelWaterManager.cpp:1442`) | binds the "スクリーンテクスチャ" EFB copy as `GX_TEXMAP0`, projects via `C_MTXLightPerspective(camera fovy/aspect)`, indirect-warps with `waterref.bti`, masks with `waterMask.bti`, sun-glint with `waterSpec.bti` | `mPerformListGXPost` (0x24) post phase `0x80`; surface silhouette/volume in the GX/silhouette lists at `0x8` | **YES** (samples the screen texture) | yes (water quads) | none (pure draw) | **expensive** — full water quad set + indirect TEV |
| 2 | **Screen-texture EFB copy** ("通常シーン描画ステージ" `TEfbCtrlTex`, set up `MarDirectorSetupObjects.cpp:387-400`) | `GXCopyTex(EFB → half-res RGB565 screen texture)`; copy phase `0x8` | `mPerformListGX` / post region, copy phase `0x8` | **YES** (is the EFB→tex copy) | no (writes a texture) | none | **cheap-ish** (one EFB-copy resolve) but it is the *source* of #1/#3/#4 |
| 3 | **Dash blur** (`TAfterEffect::perform` `0x8022d4f8`, `ScreenUtil.cpp:92`) | binds the same screen texture (`loadAfter` :45), full-screen blurred quad when `param_1 & 0x10` | post region (`& 0x10`) | indirectly (uses #2's copy) | yes (full-screen quad) | none | medium (full-screen quad) — only active during a dash |
| 4 | **Heat-haze / shimmer** (`TShimmer::perform` `0x8019f83c`, `Shimmer.cpp:28`) and **underwater filter** (`TMapObjWaterFilter::perform` `0x801ea840`) | inject the screen texture into a model material slot 1, draw a warped overlay | scene lists | indirectly (uses #2) | yes (overlay) | the shimmer SRT (`J3DFrameCtrl unk58`) advances **only under `& 1`** (calc) | medium |
| 5 | **Mirror EFB texture** ("鏡描画ステージ" `TEfbCtrlTex`, `MarDirectorInitECT.cpp:79-90`) | renders the mirror camera's view, `GXCopyTex` into the mirror texture; reused by mirror-surface models | `mPerformListGX` (0x1C) — the **whole mirror pass** is here | **YES** (EFB→tex of a re-rendered scene) | no (writes a texture) | none | **very expensive** — re-renders the scene from the mirror camera + a copy |
| 6 | **Graffiti / pollution EFB readback** (`initECTGft` → `unk38`/`unk3C`, `MarDirectorInitECT.cpp:19-73`, two `TEfbCtrlTex`) | EFB→tex of pollution layers, copy phase `0x8`, ortho re-draw `0x10` | `unk38`/`unk3C` (0x38/0x3C), branch-B draw | **YES** | writes a tex then a quad | none (visual) | medium-expensive |
| 7 | **Bath-water mist** (`draw_mist`, `BathWaterManager.cpp:6`) | `GXSetTexCopySrc`+`GXCopyTex(buffer)` of a sub-rect, then a 4-texgen warped quad | its own draw | **YES** (a sub-rect EFB copy) | yes (quad) | none | medium, localized |
| 8 | **Sun-occlusion Z peek** (`TSunModel::getZBufValue` `sunmodel.cpp:267`) | `GXPeekZ(x,y)` at 17 sample points; if Z==0xffffff the sample is "sky visible" → drives lens-glow ratio | called from `sunmgr.cpp:109` (calc) | **YES** (reads EFB depth) | no | **YES — sets `unk180[17]` (sun-visible table)** consumed by lens glow `unk48` chase | cheap GX call but **STATE-MUTATING** |
| 9 | **Mario occlusion ARGB peek** (`TMario::drawSyncCallback` `MarioMain.cpp:250`) | `GXPeekARGB(mMarioScreenPos)`; alpha==0x10 → not occluded; sets/clears `MARIO_FLAG_OCCLUDED` | a **draw-sync token callback** (`GXSetDrawSyncCallback`, `Application.cpp:231`; `DrawSyncManager.cpp:130`), NOT a perform list | **YES** (reads EFB color) | no | **YES — sets `MARIO_FLAG_OCCLUDED`** consumed by the camera (`CameraNormal.cpp:160`) and the marukage alpha chase (`DrawUtil.cpp:96`) | cheap GX call but **STATE-MUTATING + token-timed** |

### Cheap vs expensive to redo per field
- **Never redo (state-mutating, not visual):** #8 sun Z-peek, #9 Mario ARGB-peek. Re-running
  them on the in-between **double-mutates gameplay/camera state** (`MARIO_FLAG_OCCLUDED`, the
  sun-visible table) against a slightly different (interpolated) EFB → the camera and the
  shadow alpha jitter, and #9 is driven by a GPU **draw-sync token** whose timing we are not
  reproducing on the in-between. These must be suppressed on the in-between regardless of which
  strategy we pick for the visible layers. (This is *why* `interp_redraw.cpp`'s "suppress the
  EFB-feedback peek callbacks doesn't stop it" attempt was on the right track for #9 but
  doesn't address the visible water — they are different passes.)
- **Cheap-ish but pointless to redo:** #2 the EFB copy itself (the source of the flicker — see
  §2a).
- **Expensive:** #5 mirror (re-renders the scene), #1 water (full indirect-TEV quad set), #6
  graffiti, #3 dash blur, #7 bath mist.

---

## 2. Strategy evaluation

The single load-bearing fact (from `water_rendering.md` §2): **the water output is a pure
function of (the screen-texture = EFB contents at copy time, camera fovy/aspect, water vertex
positions, the static indirect warp).** Changing the EFB changes every reflected pixel; the
projection is screen-space and the warp is non-linear.

### (a) Re-render everything per field INCLUDING re-capturing the screen-texture (current)

What we do now (`interp_redraw.cpp` re-issues `kDrawLists` incl. `0x24` GXPost; the screen-tex
copy and `drawRefracAndSpec` run again against the interpolated EFB).

- **Is the flicker "wrong", or just an interpolation artifact?** It is a *real* artifact, and
  it cannot be tuned away. The interpolated EFB is produced by blending **draw matrices in eye
  space** (`j3d_draw_pipeline.md` §5) — that is a correct intermediate for *opaque geometry*.
  But the water then does a **screen-space projection** (`C_MTXLightPerspective` of the live
  camera, `ModelWaterManager.cpp:1457`) of that interpolated EFB, plus an **indirect texture
  warp** (`GXSetTevIndWarp`, :1466). Both are non-linear in the thing we interpolated, so the
  reflected pixels do not move smoothly between fields; they shimmer. Worse, the half-res
  RGB565 screen-texture re-copy of a *different* EFB each field changes the sampled content
  field-to-field even where geometry barely moved — visible as the reported per-field flicker,
  in lockstep with the shadow (which shares the silhouette draw / the same field cadence).
- **Pros:** maximal "everything at 60 fps"; no new compositing path.
- **Cons:** flickers (the user's report); pays the expensive mirror + water re-render twice;
  and it re-runs #8/#9 peeks unless separately suppressed → camera/occlusion jitter.
- **Verdict:** reject as the visible-water path. The flicker is intrinsic to interpolating an
  EFB readback.

### (b) Screen-space layers at 30 fps; carry forward the REAL field's COMPOSITED water (RECOMMEND)

Render the EFB-feedback passes (#1 water, #2 copy, #3 dash blur, #5 mirror, #6 graffiti, #7
bath, and the #8/#9 peeks) **only on real fields**. On the in-between:
1. Re-issue **only the geometry lists** (the opaque/translucent scene) with the blended draw
   matrices — i.e. drop the post list `0x24` and the EFB-readback passes from the re-issue set.
2. After the in-between EFB holds the interpolated geometry, **composite the real field's
   finished water/effect pixels on top** as a screen-aligned layer.

**How compositing works with one XFB.** SMS is single-XFB (`vi_xfb_present.md` §0), and we
already give the in-between a **distinct present address** `alt = orig_fb ^ 0x00400000`
(`interp_redraw.cpp:176`). The water region from the real field already exists — it is the
difference between the real field's pre-water EFB and its final EFB. Two faithful ways to carry
it forward, in order of fidelity/cost:

- **(b1) Re-issue the water against the REAL field's screen-texture (frozen source), with the
  water surface at the REAL field's position too.** I.e. on the in-between, run
  `drawRefracAndSpec` but (i) skip the screen-texture re-copy (#2) so it samples the real
  field's capture, AND (ii) draw the water quads at the **un-blended (tick-N) surface
  transform**, while the geometry behind is interpolated. This is the "carry forward the
  composited water region" idea: the water layer is byte-identical to the real field's water
  (same source texture, same surface verts), so it does not flicker, and it sits over the
  moved geometry. The geometry visible *through/around* the water moves at 60 fps; the water
  surface refraction is held for the field pair (30 fps), which matches hardware.
  - This is the cheapest correct option and reuses the engine's own draw path. The previously
    tried "reuse the real field's screen-texture" **ghosted** because the water surface verts
    were *interpolated* while the reflected content was frozen — the mismatch is the ghost.
    Fix: freeze BOTH the screen-texture AND the surface verts (do not blend the water
    manager's draw matrices on the in-between). They then agree; no ghost.
- **(b2) Blit the real field's final water-region pixels.** Keep a copy of the real field's
  final EFB *water region* (a texture) and, on the in-between, after geometry, draw it back as
  a masked screen-aligned quad using the same `waterMask`. More work to mask correctly than
  (b1) and offers no fidelity gain, so prefer (b1).

- **Pros:** no flicker (the water layer is identical across the field pair); no ghost (surface
  + reflected content are from the same field); faithful to hardware's native 30 fps water;
  **skips the second expensive mirror + screen-copy re-render** (perf win vs (a)); naturally
  excludes the #8/#9 peeks from the in-between (correctness win).
- **Cons:** the water/mirror/graffiti animate at 30 fps (acceptable — that's native); the
  surface is locked to tick N, so if Mario stands still and only the camera pans, the water
  refraction is one field stale relative to the panned geometry. In practice the surface is a
  near-flat plane and the refraction warp dominates the look, so a one-field hold is far less
  visible than the current flicker.
- **Verdict: recommend (b1).**

### (c) Interpolate the screen-texture / water result between the two real fields

Blend the water (or the screen texture) of real fields N and N+1 for the in-between.

- **Fatal flaw — causality/latency.** The in-between is presented *between* fields N and N+1,
  before N+1 has been simulated or drawn (`vi_xfb_present.md` §4: one `endRendering` per logic
  frame; the in-between is inserted inside frame N's `endRendering`, `interp_redraw.cpp:144`).
  We do not have N+1's water at that moment. To get it we would have to **delay presentation by
  one full game frame** (present N's real field one frame late so N+1 is available to blend) —
  that adds 33 ms of input latency and fights the engine's retrace pacing. Not acceptable for a
  platformer.
- Even ignoring latency, blending two half-res RGB565 refraction images cross-fades two
  *different* warped reflections — it dissolves rather than moves, and still doesn't track the
  60 fps geometry parallax.
- **Verdict:** reject.

### (d) Render opaque+water as ONE unit, interpolate as a 2D reprojection

Treat the whole composited frame as a 2D image and warp/reproject it for the in-between.

- The water is a **transparent screen-space layer over** the opaque scene, not part of it. A
  2D reprojection of the composite moves the water and the geometry under it together, so the
  parallax between the surface and what it refracts is lost — defeating the purpose (and the
  refraction warp would smear). A reprojection also needs per-pixel motion vectors / depth we
  do not synthesize.
- **Verdict:** reject. (Note: object-level matrix blend, which we already do, is the *correct*
  form of "reproject the geometry" — but it operates on geometry, not on the composited 2D
  frame, and explicitly excludes the screen-space layer, which is exactly the split (b) makes.)

---

## 3. Recommended approach — concrete implementation

**Adopt (b1): geometry at 60 fps via the existing draw-matrix blend; the EFB-feedback layer
held at 30 fps by re-drawing the water from the real field's frozen screen-texture + frozen
surface, and by NOT re-running the readback/peek passes on the in-between.**

### 3.1 What changes in `interp_redraw.cpp`

Today the in-between re-issues `kDrawLists = { 0x40, 0x38, 0x3C, 0x1C, 0x20, 0x24 }`
(`interp_redraw.cpp:47`) — that includes the mirror pass (`0x1C`), the graffiti EFB passes
(`0x38`/`0x3C`), and the post list (`0x24`) which carries the water refraction and the
screen-texture copy. Change the in-between to re-issue only the **geometry** of the scene:

1. **Drop the EFB-feedback lists from the in-between re-issue set.** Re-issue the main scene
   geometry (`unk34`, the preEntry list — confirm it is in the path; `perform_list_architecture.md`
   §0 warns the bulk of the scene is `unk34`, not the named GX list) plus the opaque/silhouette
   geometry, but **skip** `0x24` (GXPost / water refraction / lens), and skip `0x1C` (mirror
   re-render) and `0x38`/`0x3C` (graffiti EFB readback). These are the EFB-feedback passes; they
   stay on the real field only. (`list_mask` already exists for this bisection,
   `interp_redraw.cpp:308-309` — make the in-between's default mask exclude the feedback lists
   rather than including them.)

2. **Force the screen-texture copy to be skipped on the in-between** so any feedback consumer
   that *does* still run reuses the real field's capture. The seam already exists:
   `ov_efbctrltex_perform` (`interp_redraw.cpp:108`) drops the `& 0x8` copy bit when
   `g_i60.skip_efbcopy`. Make that the default during the in-between (not an opt-in probe), so
   #2/#5/#6's EFB→tex copies do not re-resolve the interpolated EFB.

3. **Re-draw the water surface (#1) frozen at tick N**, after the geometry, so it composites
   over the interpolated scene without flickering or ghosting:
   - Re-issue the water object's `0x80` post phase (just the water link of the post list, or the
     whole post list with the copy bit forced off per step 2) so `drawRefracAndSpec` runs and
     samples the **real field's** screen texture (frozen by step 2).
   - **Do NOT blend the water manager's draw matrices on the in-between** — exclude the water
     manager's model from the registry blend, or restore its `mDrawMtxBuf[1][view]` to tick N
     before the in-between draw (the per-model save in `interp_capture.cpp:104-108` already lets
     us restore selectively). This keeps the surface verts at tick N so they agree with the
     frozen reflected content (kills the ghost from the earlier failed attempt). The water
     manager object is `水マネージャ`; identify its `J3DModel` instance once (probe the
     registry for the model whose viewCalc feeds the water shapes) and tag it "no-blend".
   - Similarly hold the marukage shadow (`TSilhouette`, #also-flickers) at tick N OR keep the
     existing `gpMarioPos` blend (`interp_redraw.cpp:259-282`) consistently — but tie the
     shadow's field choice to the water's: both are part of the 30 fps screen-space layer, so
     holding both at tick N is the simplest non-flickering choice.

4. **Suppress the EFB-peek state probes on the in-between (always).** These are correctness, not
   aesthetics:
   - **#9 `TMario::drawSyncCallback`** (`MarioMain.cpp:250`, vtable/addr via the existing
     `ov_silhouette`-style override seam) — gate it to no-op while `g_interp60_in_redraw` so it
     does not re-`GXPeekARGB` and flip `MARIO_FLAG_OCCLUDED` against the interpolated EFB. It is
     driven by a GPU draw-sync token (`DrawSyncManager.cpp:130`); the in-between's token timing
     is not the real frame's, so re-running it is doubly wrong.
   - **#8 `TSunModel::getZBufValue`** (`sunmodel.cpp:267`) — it runs from the **calc** path
     (`sunmgr.cpp:109`), which the in-between does NOT execute, so it is already excluded. No
     action needed beyond *not* adding the sun manager to the in-between. Documented so a future
     change doesn't accidentally pull it in.

### 3.2 The buffers / addresses involved

- **Present address (unchanged):** in-between presents to `alt = orig_fb ^ 0x00400000`, copy
  dest steered to `alt` via `display+4`/`display+8`, `setNextXFB(alt)`
  (`interp_redraw.cpp:176,198,337-339`). Keep exactly as is — this is the verified RBRB cadence.
- **Screen-texture (frozen source for the water):** the "スクリーンテクスチャ" `JUTTexture`
  cached at `TModelWaterManager+0x5D34` (`ModelWaterManager.cpp:194`), filled by the
  "通常シーン描画ステージ" `TEfbCtrlTex` `GXCopyTex` (`MarDirectorSetupObjects.cpp:387-400`).
  On the in-between we **do not overwrite it** (step 2), so the water samples the real field's
  contents. No new buffer needed — we are *reusing* the engine's own copy and the engine's own
  water draw, just with the copy and the surface-verts frozen.
- **Water surface draw matrices:** `mDrawMtxBuf[1][view]` of the `水マネージャ` model — hold at
  tick N (do not blend); use the existing per-model save/restore.

### 3.3 Override addresses / seams (all already present or trivially added)

| Need | Seam | Address |
|------|------|---------|
| insert the in-between, present to alt | `ov_interp_endRendering` (exists) | `TDisplay::endRendering` `0x802f80d0` |
| re-issue geometry lists only (exclude feedback lists) | `kDrawLists` / `list_mask` (exists) | `TPerformList::perform` `0x802a4e28` |
| freeze the screen-texture copy on the in-between | `ov_efbctrltex_perform` (exists; make default) | `TEfbCtrlTex::perform` `0x802f8bac` |
| hold the water surface at tick N | per-model no-blend tag in the registry | `interp_capture.cpp` blend list; water model = `水マネージャ` J3DModel |
| suppress Mario occlusion peek on in-between | new override gated on `g_interp60_in_redraw` | `TMario::drawSyncCallback` (resolve addr from `MarioMain.cpp:250` / vtable) |
| keep sun Z-peek excluded | do NOT re-issue the calc/sun path | (already excluded — `sunmgr.cpp:109` is calc) |

### 3.4 Expected visual result and tradeoff
- **Geometry, camera, Mario, NPCs, world: smooth 60 fps** (unchanged from today's geometry
  blend).
- **Water refraction, mirror, graffiti readback, dash blur, shimmer: stable 30 fps** — held for
  the field pair, exactly as on hardware. **No flicker, no ghost.**
- **Occlusion-driven state (camera framing, shadow alpha, lens glow): stable** — the peeks run
  once per real field as designed, not twice against an interpolated EFB.
- **Residual:** when the camera pans fast while the water surface is static, the held water is
  up to one field (~16 ms) stale relative to the geometry under it. This is far less noticeable
  than the current per-field shimmer, and is the same cadence the real console shows. If it ever
  matters, the *surface* (not the reflected content) could be cheaply 2D-shifted by the camera's
  screen-space delta — but defer that; it is gold-plating until the user sees the residual.

---

## 4. CONCLUSION

**Implement (b1).** The water (and every EFB-feedback effect) is a screen-space readback of the
EFB; interpolating that readback is intrinsically wrong (non-linear screen-space projection +
indirect warp + half-res re-copy of a different EFB each field = the reported flicker), and
true temporal interpolation (c) is impossible without a frame of latency we can't spend. The
correct PC-port split is: **interpolate geometry to 60 fps (we already do), and present the
EFB-feedback layer at its native 30 fps by holding the real field's water — frozen screen
texture AND frozen surface verts together (so it neither flickers nor ghosts) — and by never
re-running the EFB peeks on the in-between.**

The exact engine seams: keep the present-to-`alt` machinery (`endRendering` `0x802f80d0`);
narrow the in-between re-issue to the geometry lists (drop `0x24`/`0x1C`/`0x38`/`0x3C` from
`kDrawLists`); make `TEfbCtrlTex::perform` `0x802f8bac` drop its `& 0x8` copy on the in-between
by default; tag the `水マネージャ` water model "no-blend" so its surface stays at tick N to match
the frozen reflection; and add a `g_interp60_in_redraw` gate on `TMario::drawSyncCallback`
(`MarioMain.cpp:250`) so the occlusion peek runs once per real field only. The sun Z-peek
(`TSunModel::getZBufValue`, `sunmodel.cpp:267`) is already excluded because it lives in the calc
path the in-between doesn't run — keep it that way.

### Open / to verify at implementation time
- The exact perform-list membership of `水マネージャ`'s `0x8`/`0x80` water draw and of
  `TSilhouette` is **data-driven** (`/data/PerformLists.bin`) — confirm at runtime which list
  the water refraction link lives in before narrowing `kDrawLists`, per
  `perform_list_architecture.md` §6 (dump each `TPerformList`'s links + filters). The water draw
  may need to be re-issued as a single link rather than the whole post list.
- Resolve the `TMario::drawSyncCallback` runtime address / vtable slot (the override seam) — it
  is a draw-sync token callback registered via `GXSetDrawSyncCallback` (`Application.cpp:231`),
  not a perform-list entry, so it may already be firing through `TDrawSyncManager`
  (`runtime/overrides/sms_drawsync_lossproof.cpp`); check whether the in-between's GXCopyDisp
  emits a token that re-triggers it, and gate there if so.
- Confirm the in-between actually re-issues `unk34` (the main scene) today — `interp_redraw.cpp`
  uses `kDrawLists` (0x40/0x38/0x3C/0x1C/0x20/0x24), which per
  `perform_list_architecture.md` §0 does **not** include `unk34`. If geometry interpolation is
  already visibly working, the scene must be reaching the GPU another way (the registry blend
  edits the buffers the *real* field's `unk34` already wired); verify this before assuming the
  geometry path is list-driven, because (b1) relies on the geometry being re-issued/visible on
  the in-between independently of the post list.
