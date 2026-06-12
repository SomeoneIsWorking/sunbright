# Widescreen screenspace effects — inventory & status

How 16:9 works here (see `runtime/overrides/scene_render.cpp`): the GXSetProjection
override pre-squeezes every projection by 0.75 — perspective gets a wider FOV (true
Hor+), 2D orthos render anamorphically so menus present correct-aspect, CENTERED in
the 4:3 middle of the 16:9 frame. Consequence: anything that fills "the screen" with
0..640 2D geometry only covers the centre 4:3 unless handled.

The fix pattern (proven, `fader_widescreen.cpp`): scope the effect's draw with
`ws_2d_suspend_begin/end` — re-issues the last 2D ortho UNSQUEEZED (0..640 then spans
the full 16:9 present), re-squeezes on exit — plus widening any explicit fill TRect by
w/6+1 per side. Do NOT use a leaky "inside effect X" flag on a hot global hook
(GXLoadPosMtxImm): the tail-recursive scene draw leaks it (reverted TSunGlass attempt,
note in scene_render.cpp).

## Inventory (from reference/sms decomp) and status

| Effect | Guest func | Status |
|---|---|---|
| TSMSFader fades (incl. TShineFader — inherits draw) | drawFadeinout 0x8013fa54, draw 0x8013fc88 | ✅ d22b78d / 516ca66, `fader_widescreen.cpp` |
| hx_wiper circle-wipe curtain | drawn inside TSMSFader::draw scope | ✅ 516ca66 (squeeze suspend) |
| GC2D fill_rect (telop banner backdrop etc.) | 0x80140390 | ✅ 0294e7f, `fillrect_widescreen.cpp` (full-width rects only) |
| 3D actor culling at 4:3 frustum edges | SetViewFrustumClipCheckPerspective 0x802260cc | ✅ 844124c, `cull_widescreen.cpp` (aspect ×4/3) |
| TSunGlass full-screen tint/darken (pause darken, sunglasses) | draw 0x8017d354 | ✅ d4f0945, `hud.cpp` (LR-keyed matrix un-squeeze at TSUNGLASS_POSMTX_LR 0x8017d3ec — do NOT also wrap the draw entry, it would double-apply) |
| TAfterEffect dash-blur / screen-flash quad | perform 0x8022d4f8 | ✅ `screenfx_widescreen.cpp` (suspend only — quad covers viewport; the half-res screen capture is anamorphic so the stretch is correct) |
| TShimmer heat haze | perform 0x8019f83c | removed by design (`sms_widescreen.cpp`, SUNBRIGHT_KEEP_SHIMMER to restore) |
| HUD corner gauges | J2DPicture::drawFullSet | ✅ per-element edge anchoring, `hud.cpp` |

## Not screenspace (checked, no action needed)

- `TSplashManager` (Player/SplashManager.cpp): water-splash billboards in VIEW space
  (identity pos mtx applied to view-transformed positions, perspective projection) —
  covered by the widened 3D projection. The "screen droplets" are these view-space
  quads, not a 2D overlay.
- `TScreenTexture` / `SMS_FillScreenAlpha`: EFB alpha fill at ±1000 with identity
  matrices — already covers the whole EFB regardless of aspect.
- `TMovieSubTitle`: J2D text via ortho graph — centered 4:3 composition is correct.

## Dead ends / gotchas

- The earlier TSunGlass fix via an always-on GXLoadPosMtxImm hook gated on a
  `g_in_sunglass` flag was REVERTED — the flag leaked across the tail-recursive scene
  draw and right-shifted the file-select. Scoped wraps on the effect's own draw entry
  are the only safe shape.
- `ws_2d_suspend_*` actively re-issues GXSetProjection with `g_ws_last_ortho`; only
  wrap draws that actually run under a 2D ortho (verify by disasm/decomp first).
