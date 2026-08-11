# Widescreen screenspace effects — inventory & status

> **Paths in this document (reconciled 2026-08-12).** It predates the June-era layout
> reorganisation and the 60fps rewrite, so most `runtime/overrides/*.cpp` paths below name files
> that no longer exist. Where the mechanism survives, it moved:
>
> | named as | now | confirmed by |
> |---|---|---|
> | `sms-recomp/runtime/render/scene.cpp` | `sms-recomp/runtime/render/scene.{h,cpp}` | holds the `GXSetProjection` (0x80362c34) hook |
> | `sms-recomp/runtime/devices/dev_gxfifo.cpp` | `sms-recomp/runtime/devices/dev_gxfifo.cpp` | the gather-pipe route |
> | `sms-recomp/overrides/hud.cpp` | `sms-recomp/overrides/hud.cpp` | same file, qualified |
>
> `runtime/overrides/scene_id.cpp` and its `SUNBRIGHT_2DID` switch are GONE — no such file and no
> such switch is read anywhere in the tree. Treat that paragraph as a record of what was once
> built, not as a tool you can run.

How 16:9 works here (see `sms-recomp/runtime/render/scene.cpp`): the GXSetProjection
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

---

# Recomp-era addendum (2026-07-23)

The table above was written for the Dolphin-era runtime. Everything it marks ✅ has now been
ported to `sms-recomp` (`widescreen.cpp`, `widescreen_effects.cpp`, `hud.cpp`,
`sunmodel_widescreen.cpp`). Three things do NOT carry over unchanged, all for the same reason:

**"A fix exists" and "the fix runs" were different claims in the Dolphin era.** That runtime
reached overrides through JIT block-linking, which silently skipped some entirely — this document's
own dead-ends section records the TAfterEffect/TEfbCtrlTex fixes never being in the binary at all.
In the recomp EVERY call goes through `call_ppc`, so every override fires, and fixes that were
never exercised are exercised now.

Hence `/wsfx`, a live effect census on the probe (`SBR_PROBE=1`): which widescreen-affected effects
actually ran this session, and how often. Measure before believing this table.

## Corrections found by measuring, not reading

1. **The fade rect was widened TWICE.** `TSMSFader::draw` widens and then calls `drawFadeinout`,
   which is also hooked; both reach `fill_rect`. Measured via `/fills`: the same rect arriving as
   both `-107..747` and `-250..890`. Only the outermost wrapper may widen now.

2. **The fader's suspend scope ran every frame while drawing nothing.** `TSMSFader::draw` returns
   immediately when `mFadeStatus == FULLY_FADED_IN` (all of normal gameplay), but the wrapper still
   entered `ws_2d_suspend_begin/end`, which re-issues a projection on entry AND exit — 4860 scoped
   pairs per run in Delfino Plaza where 37 were wanted. Now mirrors the game's own early-out
   (status at +0x20).

   NOTE, since the tempting theory is wrong: this was NOT clobbering a perspective projection.
   Measured `fader.draw.under2d = 4860 of 4860` — the fader always runs under a 2D ortho, so the
   reload re-issued the same kind of projection. It was waste and a latent hazard, not corruption.
   The same check says `TAfterEffect` also always runs under 2D, as this document assumed.

3. **`TMovieSubTitle: centered 4:3 composition is correct` describes the wrong element.** The
   subtitle band that is narrower than its text (user report, pre-existing since the Dolphin era) is
   drawn by **`J2DTextBox` panes `tet1`/`tet2`**, identified live via `/2dclass` — the telop/message
   system, not `TMovieSubTitle`'s `me_a`/`me_b`. It is also NOT `fill_rect` (`/fills` shows only
   fades) and NOT the quad emitter (`/2d` does not list it). STILL OPEN: the band pane itself has
   not been identified — it is neither a J2DPicture nor a J2DTextBox, so the next step is
   `J2DPane::drawSelf`, the base-class path this diagnostic does not yet hook.

## Diagnostics (all live, all on the probe)

| Endpoint | Answers |
|---|---|
| `/wsfx` | which widescreen effects ran, how often, and under which projection kind |
| `/fills` | every distinct rect `fill_rect` was asked for |
| `/2d` | 2D panes drawn via the quad emitter: name, transform, HUD anchor |
| `/2dclass` | which J2D CLASS drew each pane (needs `SBR_DIAG_2D=1`) |

## Not yet verified in this runtime

`sunmodel_widescreen.cpp` is ported but has never been observed executing — `getZBufValue` was not
reached in Delfino Plaza or Gelato Beach across ~1000 frames. It needs a scene with a live sun/lens
flare before its correction can be called verified.

## Heat-haze ghosting — the screen-projected effect matrix (2026-07-23)

User report: "haze ghosting ... it's that they are positioned wrong ... actually port these, not
tweak them." Correct on all counts. A texture-format tweak (R8->I8) did NOT fix it; the ghost was a
positioning bug in a screen-projected effect.

Bisected on real data: widescreen off = clean; SBR_WS_SCALE=1.0 (structure on, no squeeze) = clean;
mirror suspend on/off/squeeze = no change (mirror ruled out); skipping TShimmer::perform (0x8019f83c)
= clean. So the ghost is the HEAT HAZE, and it is the squeeze that triggers it.

Root cause: SMS_GetLightPerspectiveForEffectMtx (0x8022ba74, MtxUtil.cpp) builds a projected-texgen
"effect" matrix from gpCamera->getFovy()/getAspect() — the camera's TRUE aspect. The screen-effects
that project a screen-capture back onto geometry use it to find where each vertex rasterized so they
can sample the screen there: TShimmer (heat haze), water refraction, DebuTelesa's ghost distortion,
and several MapObj/NPC effects. Under the anamorphic squeeze the real render scales horizontal
projection by 0.75, but this matrix stays unsqueezed (the squeeze only touches the packed GX
projection, never gpCamera). So the effect sampled screen-U at unsqueezed x while the geometry sat at
squeezed x — the whole captured scene showed up shifted sideways through the "distortion" = ghost.

Fix (widescreen_effects.cpp, ov_effect_mtx): override the function, run the real body, then multiply
row 0's x-scale (m[0][0]) by the same 0.75 the main render uses. This is porting the effect to
widescreen — making its projection consistent with the anamorphic render — NOT the retired approach
of deleting the haze. One override fixes every consumer, because they all read this one matrix.

Verified: ghost gone with the haze STILL running (matches the shimmer-skipped baseline but with the
effect present); widescreen off unaffected (override no-ops).

NOTE: the mirror (TMirrorCamera + C_MTXLightPerspective) is a DIFFERENT projected-texgen path and was
ruled out for THIS ghost, but it has the same structural risk (lookup built from unsqueezed camera
params, surface drawn in the squeezed view). No reflection ghosting is currently observed; revisit
with evidence, not pre-emptively.

## Widened at the INPUT, not the output (2026-07-23) — the actual port

The heat-haze ghosting forced the realising insight: the whole "squeeze the projection output, then
patch each screen effect" approach was backwards. It was patching, not porting.

3D widescreen now happens at the ONE shared input — the aspect passed to `C_MTXPerspective`
(0x8034a404). Measured: a single aspect (~1.346) flows through it, for the main camera AND for
`SMS_GetLightPerspectiveForEffectMtx` (the projected-texgen matrix every screen effect rebuilds).
Widen the aspect there (× (16:9)/(4:3)) and the main projection and every effect come out 16:9
CONSISTENTLY — there is nothing to patch per effect, because they all read this.

This DELETED, as redundant:
- the perspective squeeze at GXSetProjection (now only 2D ortho is squeezed there — ortho is built
  by C_MTXOrtho, has no aspect, and cannot be widened this way);
- `ov_effect_mtx` (the per-effect matrix patch that briefly fixed the heat haze by squeezing the
  effect matrix's m[0][0] — now the effect matrix is widened at its own C_MTXPerspective call);
- `sunmodel_widescreen.cpp` (recomputed the sun's EFB occlusion pixels *because* the projection was
  squeezed after the fact; with the projection wide at the source the sun projects correctly on its
  own, and the recompute would double-apply).

`g_ws_persp_suspend` now gates the C_MTXPerspective widen (was: the GXSetProjection perspective
squeeze) — same semantics, so the mirror pre-render still renders at the un-widened aspect to match
its own lookup. STILL NEEDED and unchanged: the 2D ortho squeeze, the cull widen (separate path,
does not go through C_MTXPerspective), HUD anchoring, the full-screen 2D effect widenings, and the
EFB-tex/mirror suspends.

Verified: Delfino Plaza renders wide with NO heat-haze ghost and the effect still running;
file-select correct (wide, centred menus, Mario un-stretched); widescreen off untouched.
