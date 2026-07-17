// sms_boot_wave_grid.h — pure spec for TMapObjWave's grid geometry + wave math.
//
// The reflective sea (title screen) is drawn as a Mario-centred grid of triangle strips.
// Faithful to the RE (@0x801dcc08 load / @0x801dd21c draw / @0x801dce60 updateTime /
// @0x801dd694 getWaveHeight — decomp/sms/src/MoveBG/MapObjWave.cpp). All values are
// SDA2 constants:
//   mExtentBase   = 5200.0f (SDA2[-0x2510])
//   mGridStep     =  200.0f (SDA2[-0x250c])
//   mHalfExtent   = mExtentBase * 0.5f          = 2600
//   mInvHalfExtent= 1.0f / mHalfExtent          = 1/2600
//   mStripCount   = (s32)(mExtentBase / mGridStep) = 26
// Per strip: 26 (x cells) * 2 (near+far vert per column) = 52 verts. Total: 26 * 52 = 1352.
// This is the ground-truth vertex count the oracle draws (SUNBRIGHT_DBG_GXTEV attribution
// on the settled title-screen frame: 26 draws × 52 verts, tev=2 SRCALPHA/SRCCLR pass3).
//
// The tested functions ARE the shipping functions (MapObjWave.cpp calls them directly),
// so a broken test flags a real regression rather than validating dead code.

#pragma once
#include <cmath>
#include <cstdint>

namespace sb::wave {

struct GridDims {
    float half_extent;      // mExtentBase * 0.5
    float inv_half_extent;  // 1 / half_extent
    int   strip_count;      // (int)(extent / step)
    int   verts_per_strip;  // strip_count * 2 (near+far per column)
    int   total_verts;      // strip_count * verts_per_strip
};

// Derive the grid dimensions from the two SDA2 inputs. Faithful to load()'s casts:
// mStripCount is truncated to s32 (RE: `stw r0, 0x1C(r3)` after a `fctiwz` of the fdiv).
inline GridDims grid_dims(float extent_base, float grid_step) {
    GridDims g{};
    g.half_extent     = extent_base * 0.5f;
    g.inv_half_extent = 1.0f / g.half_extent;
    g.strip_count     = (int)(extent_base / grid_step);
    g.verts_per_strip = (g.strip_count & 0x7FFF) * 2;   // matches draw() `stripVerts`
    g.total_verts     = g.strip_count * g.verts_per_strip;
    return g;
}

// Per-vertex radial edge-fade alpha, computed at grid-local (xc, zc) — the Mario-relative
// coord BEFORE origin offset (RE: draw() lines 268-275). The alpha runs opaque at the
// centre and fades to zero at the corner of the grid. Clamped [0, 1] before scaling.
//
// alpha_ratio = 1 - inv_half * max(|xc|, |zc|)
// aN = clamp(alpha_ratio, 0, 1) * alpha_full
inline float fade_ratio(float xc, float zc, float inv_half) {
    float ax = std::fabs(xc), az = std::fabs(zc);
    float r  = 1.0f - inv_half * (ax > az ? ax : az);
    if (r < 0.0f) return 0.0f;
    if (r > 1.0f) return 1.0f;
    return r;
}
inline uint8_t fade_alpha(float xc, float zc, float inv_half, float alpha_full) {
    return (uint8_t)(alpha_full * fade_ratio(xc, zc, inv_half));
}

// updateTime()'s per-frame phase advance (RE: @0x801dce60 lines 126-129). Wraps at 2π.
// The wrap is a subtract-once (not fmod) — the game trusts that freq << 2π, so a single
// subtract restores the fundamental. Faithful to that: don't fmod, do the one subtract.
inline float phase_advance(float phase, float freq) {
    constexpr float kTwoPi = 6.28318f;  // SDA2[-0x24cc]
    phase += freq;
    if (phase > kTwoPi) phase -= kTwoPi;
    return phase;
}

// getWaveHeight(x, z) — the two-axis sinusoid draw() writes into Y per vertex (RE:
// @0x801dd694 lines 313-315). Same formula shared by the surface probe (getHeight) and
// the vertex y in draw().
inline float wave_height_2d(float x, float z,
                            float amp_x, float freq_x, float phase_x,
                            float amp_z, float freq_z, float phase_z) {
    constexpr float kInv2Pi = 0.15915507f;   // SDA2[-0x24b8]
    return amp_x * std::sin(freq_x * (kInv2Pi * x) + phase_x)
         + amp_z * std::sin(freq_z * (kInv2Pi * z) + phase_z);
}

}  // namespace sb::wave
