#pragma once
// screen_effects — the owned registry of screen-space effects (docs/screen_effects.md).
//
// The recomp runs the game's real effect code, so nothing here reimplements an effect. What it owns
// is IDENTIFICATION: which screen-sampling effects fired this frame, named, so that code which
// interacts with them — interp60's in-between replay, widescreen — acts on a known set instead of
// hunting draws in an opaque GX stream. Each effect's guest `perform` is hooked (screen_effects.cpp)
// and reports here; consumers query the per-frame set.

#include "intrinsics.h"

enum class ScreenEffect : u32 {
    Shimmer         = 1u << 0,   // TShimmer     0x8019f83c — heat haze, samples the screen capture
    DashBlur        = 1u << 1,   // TAfterEffect 0x8022d4f8 — motion-blur trail, samples the capture
    WaterRefraction = 1u << 2,   // TModelWaterManager::drawRefracAndSpec 0x8027c12c
    BathMist        = 1u << 3,   // TBathWaterManager::draw_mist 0x801aa6cc — own EFB round-trip
    MirrorPreRender = 1u << 4,   // TMirrorCamera 0x80193fbc — a second scene render into a texture
};

// Effects whose output distorts the shared screen capture (TScreenTexture). On interp60's in-between
// field these sample a capture that no longer matches the blended geometry, so the in-between
// handler must freeze the capture or re-derive them. The self-contained ones (BathMist,
// MirrorPreRender) are excluded — they produce their own texture each field.
constexpr u32 kScreenSamplingEffects =
    (u32)ScreenEffect::Shimmer | (u32)ScreenEffect::DashBlur | (u32)ScreenEffect::WaterRefraction;

// One effect fired this frame. `drew` distinguishes "perform ran" from "perform ran AND emitted its
// draw" (several effects early-out without drawing) — only a draw actually touches the screen.
void sb_screen_effect_fired(ScreenEffect e, bool drew);

// The effects that DREW this frame, as a bitmask of ScreenEffect. Cleared each frame by
// sb_screen_effects_frame_end().
u32 sb_screen_effects_this_frame();

// True if any capture-sampling effect drew this frame — the one bit interp60 needs to decide whether
// the in-between must handle the stale capture at all.
bool sb_screen_sampling_active();

// Call once per presented frame (from the frame seam) to roll the per-frame set over.
void sb_screen_effects_frame_end();
