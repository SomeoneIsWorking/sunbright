# Screen-space effects — the owned catalog

The point of this file: when a screen effect misbehaves (a widescreen ghost, an interp60 half-step
artifact), we go straight to a known mechanism instead of hunting through an opaque GX stream. Every
entry is RE'd from `decomp/sms`, with the guest address so it can be hooked in the recomp.

## The capture: `TScreenTexture` — one screen copy per frame

`TScreenTexture` (`gpScreenTexture`, NameRef `スクリーンテクスチャ`; ScreenUtil.cpp:215) allocates a
**half-resolution RGB565** `JUTTexture` — `SMSGetGameRenderWidth()/2 × SMSGetGameRenderHeight()/2`,
i.e. 320×224. The scene graph's normal-scene `TEfbCtrlTex` pass copies the rendered EFB into it
(this is the `src 640x448 → dst 320x224 fmt RGB565` copy the recomp fifo logs). Effects that want to
distort "the screen" bind THIS texture, by name, via `TScreenTexture::replace` /
`JUTTexture::setResTIMG`.

Every consumer below samples this one capture, so it is the single resource interp60 must reason
about: it is produced ONCE, from the real field's EFB, and the in-between field samples the same
bytes.

## The shared projection: `SMS_GetLightPerspectiveForEffectMtx` (0x8022ba74)

Builds the projected-texgen "effect" matrix — the camera perspective (from `gpCamera` fovy/aspect)
with the depth row replaced by `{0,0,-1,0}` so a vertex's world position maps to the SCREEN position
where it rasterized. That is how a distortion mesh knows where to sample the capture. Every
screen-projected effect rebuilds it, which is why widening the camera aspect here (widescreen) fixes
all of them at once — see `docs/60fps/widescreen_effects.md`.

## The consumers

| Effect | Guest addr | What it does | Samples capture? | interp60 note |
|---|---|---|---|---|
| **Heat haze** `TShimmer::perform` | 0x8019f83c | Distortion-mesh BMD (`J3DMLF_MaterialUseIndirect` + a BTK scroll animation) drawn with the effect matrix on texmtx(1) and the screen capture bound to texmap 1; an indirect texture displaces the sample = shimmer. `loadAfter` is where it binds `スクリーンテクスチャ`. Positions the mesh at z=9600 (distant ground) or z=0. Skips while FLUDD is emitting. | YES (texmap 1) | Samples the real-field capture at the in-between's blended mesh positions → the wobble swims unless the capture is frozen or re-derived. |
| **Dash blur** `TAfterEffect::perform` | 0x8022d4f8 | Full-viewport fan textured with the capture, offset/scaled by `calcDashBlurValue` (the motion-blur trail when Mario dashes/rockets). Gated on `unk14 & 1` (enabled) and `param_1 & 0x10` (draw pass). | YES | Its whole purpose is a trail of PRIOR frames; on the in-between it must not double-accumulate. Freeze on the in-between. |
| **Water refraction** `TModelWaterManager::drawRefracAndSpec` | 0x8027c12c | The reflective/refractive water surface sampling the capture through the effect matrix (`C_MTXLightPerspective`). | YES | Re-derive at the interpolated camera (the retired interp60 did this by re-issuing the water quad with the N½ view). |
| **Bath mist** `TBathWaterManager::draw_mist` | 0x801aa6cc | `GXCopyTex`s the EFB viewport to a texture and redraws it over the same region through its own EFB-pixel ortho — a self-contained EFB round-trip, NOT the shared capture. | its own copy | Self-contained per field; safe to let the replay re-run. |
| **Mirror pre-render** `TMirrorCamera::perform` | 0x80193fbc | Renders the scene from a mirrored viewpoint into a 256×256 texture (a SECOND scene render), sampled by reflective surfaces via `C_MTXLightPerspective`. Not the shared capture. | its own render | A second geometry pass — interp60 would have to blend its matrices too, or freeze the reflection. |

## Why this matters for interp60

interp60 presents each 30 Hz tick twice, the second with draw matrices blended toward the previous
tick, by REPLAYING the frame's GX stream (that file is GONE — the stream replay is now `extern/aurora/lib/gfx/interp.cpp`, driven from `sms-recomp/frame_interp/`). The geometry
moves on the in-between field; the **screen capture does not** (it was produced once, from the real
field's EFB). So every effect in the table that samples the capture will, on the in-between, distort
a screen that does not match the blended geometry — the artifact the retired
`efb_interp_freeze.cpp` / `shadow_interp.cpp` were built to handle.

Owning this means interp60 does not guess: `sms-recomp/frame_interp/effects.h` hooks each of (formerly `overrides/screen_effects.cpp`; the hook addresses 0x8019f83c / 0x8027c12c are what identify it)
these and records, per frame, which screen-sampling effects fired (queryable at `/screenfx` and via
`sb_screen_effects_this_frame()`), so the in-between handler acts on a named set — freeze the capture
for the shimmer/blur, re-derive the water at the interpolated camera — instead of pattern-matching
draws in the stream.
