# THP movie → ngx compositing — port plan (RE done, ready to implement)

2026-06-18. The ~45% black region during the Delfino entrance (and every THP cutscene) under ngx
present is the THP movie: the game draws the decoded video via GX, which ngx skips (ngx owns the
present, Dolphin's GX output is discarded). Port = capture the movie frame and composite it natively.
This is the most bounded piece of the "own-the-GPU / EFB" frontier (the only remaining native-engine
gap with visible missing features — the re-grounding of owned subsystems is complete: render/audio/card).

## The draw path (RE'd via sunbright-recomp --xref / --callees)
- `THPPlayerDrawCurrentFrame` (0x8001ea34) — high-level "draw current movie frame"; called from one
  site (snapGXTime/TTimeRec region 0x802a9900 — the movie director frame tick).
- `THPGXYuv2RgbSetup` (0x8001e00c) — builds the GX state: GXSetProjection(ortho)/Viewport/Scissor,
  ZMode/Blend/ColorUpdate, and a **4-TEV-stage YUV→RGB conversion** (GXSetTevOrder/ColorIn/ColorOp ×4,
  with the BT.601-ish conversion coefficients loaded as **GXSetTevKColor ×3** + GXSetTevColorS10). The
  3 tex maps are the Y, U, V planes.
- `THPGXYuv2RgbDraw` (0x8001de28) — loads **3 textures** (3× GXInitTexObjLOD + GXLoadTexObj, the Y/U/V
  planes) then **GXBegin** draws one quad (fullscreen). This is THE seam to capture.
- `THPGXRestore` (0x8001e4f0) — restores GX state after.

## ngx port approach (faithful, pragmatic)
ngx already has: purejit-safe GXLoadTexObj tee (captures tex objects), a texture upload+decode path,
and the present overlay composite (HUD quads). Reuse them:
1. **Capture seam**: make `THPGXYuv2RgbDraw` (0x8001de28) a purejit-safe override (return-true). In it,
   read the 3 bound tex objects (Y,U,V plane base/dims/format — I4/I8 intensity planes; U,V are
   half-res) straight from the GX tex-obj args / guest THP player struct, and the quad's screen rect.
   Publish a "THP frame" record (3 plane pointers+dims + dest rect) to ngx_present at the frame
   boundary (like g_pollution_pub). Skip the guest GX draw (ngx owns present).
2. **YUV→RGB in an ngx shader**: upload the 3 planes as R8 textures; a fullscreen-quad fragment shader
   samples Y (full res), U,V (half res) and applies the standard THP/BT.601 matrix:
   R=Y+1.402(V-128); G=Y-0.344(U-128)-0.714(V-128); B=Y+1.772(U-128). (The exact coefficients are in
   the captured KColors if bit-exactness is wanted, but the standard matrix is what THP encodes.)
   Draw it FIRST in the present (under the 3D scene? — no: the movie REPLACES the scene, so draw it as
   the base layer when a THP frame is active, before/instead of the 3D pass), then HUD over it.
3. **Gate**: only when a THP frame was published this frame (movie active). Otherwise normal 3D present.
4. **Verify**: AUTOSTART → Delfino entrance; the previously-black movie region shows the video.
   ab_diff vs the recomp/Dolphin-GX path is NOT valid (GX XFB black under ngx) — verify by the black
   region filling with plausible video (non-black coverage in the movie rect) + no crash.

## Gotchas anticipated
- The plane textures are intensity (I4/I8); U/V are half-resolution (4:2:0 or 4:2:2 — check the dims
  the setup loads). Sample with the right scale.
- THPGXYuv2RgbDraw runs on the movie/decode thread cadence; publish double-buffered (like the J3D
  capture) so the present reads a complete frame.
- The decoded plane buffers live in guest RAM (THP decoder output); read them via the memory bridge.
- Entry/exit: when the movie ends (THPGXRestore / player stop), clear the published THP frame so the
  present returns to 3D.

## Files to touch
- runtime/overrides/ngx_j3d_shape.cpp (or a new thp_native.cpp): the THPGXYuv2RgbDraw capture tee +
  purejit-safe mark + published THP-frame accessor (sb_ngx_get_thp_frame).
- runtime/render/ngx_present.cpp: a THP base-layer pass (3-plane YUV upload + YUV→RGB shader) gated on
  an active published frame.
- runtime/render/shaders/: thp_yuv.{vert,frag}.glsl + the spv headers.
