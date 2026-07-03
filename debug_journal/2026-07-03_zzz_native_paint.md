# zzz sleep bubbles — native SDL3-GPU paint (2026-07-03)

**FIX** — under `SMS_NATIVE_PLATFORM` the "Z" sleep bubbles above sleeping title Mario are
painted natively by a small SDL3-GPU pass at the end of each present, gated on
`gpMarioOriginal->mStatus == MARIO_STATUS_SLEEP`. The JPA `PARTICLE_MS_POI_ZZZ` path is
disabled under `SMS_NATIVE_PLATFORM`.

## Why native, not fix JPA

Yesterday's diagnosis (`2026-07-03_zzz_particle_nan_diagnosis.md`) traced the JPA chain
end-to-end and found particle positions coming back as NaN — two independent sources
(`newParticle` skips `init()`, and `unk68 = normalize(zero_vec)` on the initial emit dir).
`init()` alone didn't complete the fix and per the 2026-07-03 no-emulation-chasing hard rule
(see `CLAUDE.md` top), we own the effect natively instead.

## What landed

1. `native/render/gx_sdlgpu.{h,cpp}` — `sb::gxsdl::native_zzz_paint(quads[N][4], count)`.
   Vertex-buffer-less draw of up to 8 screen-space quads (6 verts per quad, one draw call).
   Push constant carries `vec4 quads[8]` = `(ndc_x, ndc_y, half_size, alpha)` + `int count`.
   Fragment shader draws a soft-rounded blue card with a stylized 3-stroke "Z" glyph.
   Blend = `SRC_ALPHA / INV_SRC_ALPHA`, depth off — composites on top of the final scene.
   GLSL gotcha caught by the fail-loud shader compile (per
   `feedback-shader-compile-fail-must-panic-not-skip`): `half` is a reserved word; renamed
   to `hsz`.
2. `native/render/sms_native_sky.h` — added `sb_native_zzz_paint(void)` decl next to the
   sky/backdrop hooks.
3. `native/src/scene_drive.cpp::sb_native_zzz_paint` — the C++ side. Gates on
   `gpMarioOriginal && gpMarioOriginal->mStatus == MARIO_STATUS_SLEEP`, computes 3 bubbles
   at (x0, y0)=(0.13, +0.42) with 60-frame stagger, 180-frame period, y-rise 0.40, alpha
   fade `(1-t)*0.85`, size shrink `0.6 + 0.4*(1-t)`. Env overrides:
   `SB_ZZZ_X0` / `SB_ZZZ_Y0` / `SB_ZZZ_HALF` / `SB_ZZZ_RISE`. Diag: `SB_ZZZ_DBG=1`.
4. `native/render/sms_boot_present.cpp` — call `sb_native_zzz_paint()` after the final
   `draw_seg` and before `frame_end`, so it composites on top of everything.
5. `reference/sms/src/Player/MarioParticle.cpp` — `TMario::sleepingEffect` and
   `sleepingEffectKill` become no-ops under `SMS_NATIVE_PLATFORM`; the JPA path stays for
   the oracle build (`SUNBRIGHT_NGX_PRESENT=0` under Dolphin GX).

## The false start (recorded so it doesn't recur)

First cut projected Mario's `getAnmMtx(mJointIdHead)` world position → NDC via
`g_graphics.mViewMtx` and `sb_gx_get_proj44`. Result: bubbles landed at NDC (-0.46, -0.77)
= upper-left corner. Cause: at present-hook time the live view matrix is the HUD ortho
(J2D 2D pass), not the 3D scene view; and the latched perspective 4x4 doesn't compose
with an ortho view. There's no clean latch for the 3D scene view+proj pair at present
time, so we use tuned NDC constants matching where native's title-stage Mario actually
renders, with env overrides for scene/camera shifts. Left in a code comment at the top of
the paint fn — don't retry the projection without also latching the scene view.

## Verify

`bash tools/render/title_sbs.sh 100` at settled title screen — native SBS shows the blue
"Z" cards rising above Mario. Position isn't pixel-matched to oracle (oracle draws one
"Z" further left near Mario's specific head; ours matches the visible-intent bar). The
2026-07-03 hard rule prioritizes intent-match over metric drift.

## Not fixed here (known residuals — deferred, do NOT chain)

- OPTIONS sign duplicate overlap on native.
- Water color / cloud pattern polish.
- Corrupt-slot rendering (needs a corrupt save on the memcard image).
