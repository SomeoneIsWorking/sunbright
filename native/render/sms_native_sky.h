// sms_native_sky.h — the SMS_NATIVE_PLATFORM sky port (CLAUDE.md 2026-07-03 hard rule).
//
// What TSky (reference/sms/src/Map/Sky.cpp) is trying to render:
//   1. Its bit-0x8 branch draws GXDrawSphere(8, 0x10) inside-out at scale 100000 with the flat
//      mat colour RGBA(0x00, 0x12, 0xEE, 0x80) and GX_PASSCLR — a solid deep-blue backdrop that
//      fills every pixel behind the textured dome. It is semantically a "clear-to-this-colour".
//   2. A camera-tracked sky.bmd hemisphere with a stage-15 slow Y-axis spin (0.035°/frame),
//      drawn as the entered MActor model.
//
// Under SMS_NATIVE_PLATFORM this file owns (1): the frame clear becomes TSky's backdrop colour
// when the active stage renders TSky, i.e. we render the intent (fill screen with the blue)
// natively via the SDL3-GPU render-pass load-op CLEAR — no TEV combiner, no GXDrawSphere,
// no 100000-unit sphere. drive_sky() drops bit 0x8 correspondingly, so the recompiled path
// stops issuing that DrawSphere and the TEV-emulator no longer runs it. Part (2) — the
// textured dome — is a follow-up (task #1) that will add a dedicated SDL3-GPU sky-dome
// pipeline; the recompiled J3D dome draw still runs in the meantime.
#pragma once
#include <cstdint>

extern "C" {

// True when this frame's active scene renders TSky (currently: gpMarDirector->mMap == 15,
// the title / file-select stage — the only place TSky's backdrop-sphere bit is set on the
// perform flag; other maps drive TSky with 0x204, sans bit-0x8, so no override is needed).
bool sb_native_sky_active(void);

// Writes the deep-blue TSky backdrop colour into `rgba` (0..1). Bit-exact against TSky's
// GXSetChanMatColor call: (0x00, 0x12, 0xEE, 0xFF). Alpha forced to 1.0 for the framebuffer
// clear (GC's 0x80 was chan-mat alpha, unrelated to the FB write).
void sb_native_sky_backdrop(float rgba[4]);

// Paint the native sky over the current SDL3-GPU offscreen colour target, when active. Draws a
// full-screen vertical gradient from a light zenith blue at the top to a haze horizon blue at the
// bottom — the visual intent of TSky's backdrop-sphere + sky.bmd dome. Under sms-boot the game's
// own sky.bmd batches project off-screen (see debug_journal/2026-07-03_sky_bmd_offscreen.md), so
// this is the ONLY thing that paints the sky. Call this right after gxsdl::frame_begin, before
// the first draw_tev_segment. No-op when sb_native_sky_active() is false.
void sb_native_sky_paint(void);

// SMS_NATIVE_PLATFORM zzz sleep bubbles pass (CLAUDE.md 2026-07-03 hard rule). Painted over the
// final composite when gpMarioOriginal->mStatus == MARIO_STATUS_SLEEP — the visual intent of the
// JPA `PARTICLE_MS_POI_ZZZ` particle above sleeping Mario's head, native-owned instead of chasing
// the JPA NaN dispatch chain (see debug_journal/2026-07-03_zzz_particle_nan_diagnosis.md and
// 2026-07-03_zzz_native_paint.md). Call between the last draw_tev_segment and frame_end.
void sb_native_zzz_paint(void);

}
