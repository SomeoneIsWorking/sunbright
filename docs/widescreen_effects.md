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

## Inventory (from decomp/sms decomp) and status

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
| TSunModel EFB occlusion probes (sun glow / lens flare visibility) | getZBufValue 0x8002ea70 | ✅ `sunmodel_widescreen.cpp` — unkB4 EFB pixels recomputed from unkF8 with the 0.75 squeeze (game samples 4:3 pixels of an anamorphic EFB; verified live: x43=0.942 → game pixel 621 vs true EFB 545) |
| BathWater draw_mist EFB copy-replay | 0x801aa6cc | ✅ `screenfx_widescreen.cpp` — squeeze suspended (flag only): its own EFB-pixel ortho (bl 0x801aac30 → GXSetProjection) must be the identity EFB mapping or the replay misaligns vs the GXCopyTex source |
| TMirrorCamera offscreen perspective (bathroom mirrors) | perform 0x80193fbc | ✅ `efbtex_widescreen.cpp` — perspective squeeze suspended (g_ws_persp_suspend): the mirror texture is sampled via drawSetting's C_MTXLightPerspective built from UNsqueezed camera params; squeezing the render but not the lookup shifts reflections ~25% |
| EFB→texture passes: graffiti/pollution counting + mirror pre-render | TEfbCtrlTex::perform 0x802f8bac (bracket 0x80…0x8) | ✅ `efbtex_widescreen.cpp` — pass-scoped squeeze suspend; their TOrthoProj orthos are TEXTURE-pixel space 1:1 with GXReadPixMetric counts / GXCopyTex rects (squeezing them shrank pollution pixel counts ~25% and smeared layer copy-backs). TOrthoProj canNOT be blanket-exempted: the same class is the screen 2D camera (0,16,600,464) which must stay squeezed. TEfbCtrlDisp::perform(0x80) force-clears the flag (leak guard) |

## Not screenspace (checked, no action needed)

- `TSplashManager` (Player/SplashManager.cpp): water-splash billboards in VIEW space
  (identity pos mtx applied to view-transformed positions, perspective projection) —
  covered by the widened 3D projection. The "screen droplets" are these view-space
  quads, not a 2D overlay.
- `TScreenTexture` / `SMS_FillScreenAlpha`: EFB alpha fill at ±1000 with identity
  matrices — already covers the whole EFB regardless of aspect.
- `TMovieSubTitle`: J2D text via ortho graph — centered 4:3 composition is correct.
- `TMario::drawSyncCallback` GXPeekARGB occlusion probe (silhouette): screen pos comes
  from `GXProject(GXGetProjectionv(...))` in boxDrawPrepare — the LIVE GX projection,
  which already holds our squeezed values → EFB-correct as-is. (Contrast TSunModel,
  which projects with the camera's stored 4:3 matrix — that one needed the fix.)
- `TLensFlare` / `TLensGlow` placement: both compute offsets from the sun's 4:3
  normalized screen pos and render the result under the SAME squeezed perspective
  projection — the 0.75 applies once to both sun and flare, so screen alignment is
  preserved. Their visibility inputs (unk191/194/calcHiddenRatio) are fixed via the
  TSunModel probe fix. Do NOT scale unkF8 itself — flare offsets would get 0.75².
- `TSunMgr` Noki-warp gate `isInBounds(0.3f)`: a small centre-screen region (|x43|≤0.3),
  far inside both aspects — unaffected.
- THP FMV: full-screen 2D under the squeeze → presented centered 4:3; movies are 4:3
  content, correct by design.
- Frustum-cull setter arg order: TNPCManager::clipEnemies *looks* like it passes
  (aspect, fovy) in the decomp, but the binary (0x8020a29c) loads f1=[cam+0x48]=fovy,
  f2=[cam+0x4C]=aspect — standard order; cull_widescreen.cpp's f2×4/3 is right for
  ALL callers (Animal/NPC/conductor/gesso/enemyAttachment/livemanager).
- JPA particles: no screen-space emitters and no CPU screen culling in SMS's JParticle
  (the "ClipBoard" is just draw state; GXSetClipMode is GPU clip) — covered by the
  widened perspective.
- MsIsInSight & friends: entity sight cones (AI), not camera/screen — must NOT be
  touched.
- TModelWaterManager::drawShineShadowVolume full-screen alpha quad: ±1000 at z=-200
  under the current (squeezed) perspective — covers the 16:9 frustum with huge margin.
- Demo letterboxing: SMS plays demos full-screen (no cinema-bar draw path found in the
  decomp) — nothing to widen.

## Dead ends / gotchas

- **The Delfino edge-smear bug report (2026-06-12) was NOT a geometry defect** — the
  committed TAfterEffect/TEfbCtrlTex fixes (26556fa) were never in the binary: `file(GLOB
  runtime/overrides/*.cpp)` had been evaluated before those files existed, so the build
  silently dropped them (zero `[screenfx]` log lines pre-fix). Root-cause fix:
  `CONFIGURE_DEPENDS` on the overrides/generated/recompiler globs in CMakeLists.txt —
  new sources now re-glob at build time, a new override can never silently drop out again.
  RE re-verified the geometry while chasing it: the screen capture is the
  "通常シーン描画ステージ" TEfbCtrlTex with src rect (0,0,renderW,renderH) — the FULL EFB
  (MarDirectorSetupObjects.cpp) — copied half-res into TScreenTexture, so under the
  anamorphic scheme the texture holds the full 16:9 frame and the full-width quad stretch
  is correct as-committed. Verified live in Delfino (probe-poked gpAfterEffect
  [r13-0x6108=0x8040E0B8] — alpha/scale forced visible, frame dumps): overlay covers the
  full 804px present, no seam at the 4:3 boundary columns (x≈100/703), and the
  horizontal-streak metric (left/bottom dy/dx) is 0.8–1.7 with the effect active vs 5–6
  in the bug screenshot (scratch/screenshots/ws_smear_bug.png vs ws_fix_*.png).
- Poking TAfterEffect live: a hidden (non-decomp) Mario-side driver rewrites
  unk50/unk5C/unk60/unk64 and holds unk15=2 every frame, and calcDashBlurValue then
  recomputes the targets — to hold a visible state, hammer the CURRENT values
  (+0x38..+0x44 offset/scale, +0x20/+0x24 alpha) and the alpha TARGETS (+0x1B/+0x1C).
- The earlier TSunGlass fix via an always-on GXLoadPosMtxImm hook gated on a
  `g_in_sunglass` flag was REVERTED — the flag leaked across the tail-recursive scene
  draw and right-shifted the file-select. Scoped wraps on the effect's own draw entry
  are the only safe shape.
- `ws_2d_suspend_*` actively re-issues GXSetProjection with `g_ws_last_ortho`; only
  wrap draws that actually run under a 2D ortho (verify by disasm/decomp first).
- Orthos issued inside a suspend scope are NOT recorded as `g_ws_last_ortho` (often
  stack-local matrices, e.g. draw_mist's — recording them would dangle the fader's
  reload pointer).
- The "通常シーン描画ステージ" normal-scene EfbCtrlTex bracket may stay open across the
  whole GX perform list; the TEfbCtrlDisp::perform(0x80) clear in efbtex_widescreen.cpp
  is load-bearing (restores the squeeze before the screen 2D groups), not just a guard.
- Headless verification: `SUNBRIGHT_STATE=<sav>` needs `SUNBRIGHT_STATE_FIELDS=0` under
  SUNBRIGHT_HEADLESS — the vi_end_field_event field counter never increments without a
  presenter, so the default 1500-field threshold never arms.
