// sms_native_sky.h — the SMS_NATIVE_PLATFORM sky port (CLAUDE.md 2026-07-03 hard rule).
//
// TSky (reference/sms/src/Map/Sky.cpp) draws two things at title (mMap==15):
//   1. Bit 0x8 — GXDrawSphere(8, 0x10) inside-out at scale 100000, flat chan-mat colour
//      RGBA(0x00, 0x12, 0xEE, 0x80), single-stage GX_PASSCLR. Semantically "clear-to-this-colour".
//   2. Bits 0x4|0x200 — MActor::viewCalc + entry that emits the sky.bmd batches:
//        DOME  (shape key 2d45a7be6c503257, 752 verts, per-vertex CLR0 blue (49,147,227),
//               NO texture) in DrawBuf AfterIndirect Xlu.
//        CLOUD STRIP (shape key 7bc0841d43634f53, 40 verts, 2-texgen TEV output
//               clamp(2·TEX(uv0)·TEX(uv1), 0, 1) with SRC_ALPHA/ONE additive blend, byte-exact
//               8×8 I4 texture) in DrawBuf Sky Xlu.
//   3. bit 0x2 — sets the sky MActor's base TR matrix to a camera-centered slow-Y-spin at title
//      (0.035°/frame), so both dome and cloud strip rotate slowly around Y.
//
// Under SMS_NATIVE_PLATFORM the port is (split across three seams):
//   - Backdrop (1) → sb_native_sky_backdrop writes TSky's exact chan-mat RGB into the SDL3-GPU
//     frame clear; drive_sky() drops bit 0x8 so the sphere is neither issued nor TEV-emulated.
//   - Dome (2a) → drive_sky() keeps bits 0x4|0x200 so sky.bmd's per-vertex-blue hemisphere
//     renders through the normal J3D scene batch stream (its CLR0 blue IS the sky base colour).
//   - Cloud strip (2b) → shader-swap: the 40-vertex batch stays in the scene stream with its
//     real material blend state, but its fragment shader is patched to kCloudStripFragGlsl
//     (a byte-exact RE'd port of the 2-stage TEV combiner reading the same 8×8 I4 pattern).
//     sms_boot_present.cpp filters the ph1 draw (off-screen EFB) and keeps the ph4 draw (visible).
//   - Spin (3) → runs through the recompiled TSky::perform's bit-0x2 branch untouched.
//
// A hand-tuned fullscreen gradient + fBm cloud shader was tried and DELETED 2026-07-04 — it was
// bandaid emulation-chasing (hand-picked endpoint colours, synthetic noise pattern) that
// duplicated the sky.bmd dome's per-vertex blue and the cloud strip's real texture pattern.
#pragma once
#include <cstdint>

extern "C" {

// True when this frame's active scene renders TSky's backdrop-sphere path (mMap == 15).
bool sb_native_sky_active(void);

// Writes the TSky::perform GXSetChanMatColor RGB (0x00, 0x12, 0xEE) into `rgba` (0..1) with
// alpha forced to 1.0 (GC's 0x80 was chan-mat alpha, unrelated to the FB clear write). This is
// the SDL3-GPU frame's clear colour when sb_native_sky_active() is true.
void sb_native_sky_backdrop(float rgba[4]);

// SMS_NATIVE_PLATFORM zzz sleep bubbles pass (CLAUDE.md 2026-07-03 hard rule). Painted over the
// final composite when gpMarioOriginal->mStatus == MARIO_STATUS_SLEEP — the visual intent of the
// JPA `PARTICLE_MS_POI_ZZZ` particle above sleeping Mario's head, native-owned instead of chasing
// the JPA NaN dispatch chain (see debug_journal/2026-07-03_zzz_particle_nan_diagnosis.md and
// 2026-07-03_zzz_native_paint.md). Call between the last draw_tev_segment and frame_end.
void sb_native_zzz_paint(void);

// SMS_NATIVE_PLATFORM native water paint (CLAUDE.md 2026-07-03 hard rule; sea RE journal
// debug_journal/2026-07-03_water_re_afterindirect_empty.md). Paints a turquoise gradient over
// the water region of the frame with a smoothstep horizon fade, when the scene renders water.
//
// What we're rendering: at title (map==15) the water is drawn on native as scene batches with
// shaderKey hi32 0x224004d9 — the option.arc stage-BMD water surface polygons (proven via
// SB_SKIP_KEY=224004d9 → native water strip becomes clean sky-blue, i.e. those batches ARE the
// water surface). Their native TEV/material combines a greenish-cyan raster (0.47,0.85,0.76)
// with a sandy-tone texture to produce dark green-teal (25, 122, 109) at y0.65-0.70, whereas
// oracle at the same y is rich turquoise (105, 187, 193). Per the hard rule (no emulation
// chasing) we do NOT re-tune the emulator; we own the pass — paint the intent natively over
// the frame after scene batches, before HUD/imm.
//
// Gradient endpoints are direct oracle pixel samples (2026-07-03):
//   horizon    (near screen y_ndc ≈ +0.10): (105, 187, 193)  — turquoise-cyan.
//   near-beach (near screen y_ndc ≈ +1.00): (129, 209, 211)  — lighter turquoise.
// Below y_ndc < +0.05 (above horizon) the paint is fully transparent so sky is untouched.
// TMapObjWave imm ripples still composite ON TOP of this via the imm/HUD render pass — the
// near-white ripple intent (RASC×2 clamped, MapObjWave.cpp:231-235) reads correctly against
// the turquoise base.
//
// Call BEFORE `draw_tev_segment` for the imm-batch tail (so scene batches under the water paint,
// HUD/imm over it). See sms_boot_present.cpp integration.
bool sb_native_water_active(void);
void sb_native_water_paint(void);

// DELETED 2026-07-03 (task #13): sb_native_cloud_paint / native_cloud_fill / kNativeCloudFs
// (fullscreen paint approach) is superseded by the SHADER-SWAP port. Cloud-strip batches
// (shaderKey 0x7bc0841d) stay in the scene stream at their natural DrawBuf AfterIndirect Xlu
// slot; their fragment shader is swapped to sb::gxsdl::kCloudStripFragGlsl, which samples the
// byte-exact 8×8 I4 pattern using real vertex-driven UVs from the material's texgens. No
// fullscreen paint, no region proxies, no composition-order plumbing.

}
