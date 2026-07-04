// sms_water_billboard.h — PURE billboard-quad corner math for TModelWaterManager::calcDrawVtx.
//
// Reverse-engineered from calcDrawVtx__18TModelWaterManagerFPA4_f (GMSE01 @0x8027e5f4, size 0x2EC).
// calcDrawVtx builds the water-splash TDLTexQuad: for each active type-1 water particle it view-
// transforms the particle centre + velocity, then emits a screen-facing quad — an axis-aligned
// square for slow particles, or a quad STRETCHED along the (view-space) velocity for fast ones.
// This header holds ONLY that per-particle corner computation, as plain floats (no Dolphin/GX/ROM),
// so it is unit-testable against hand-derived expected values (native/platform/tests). The shipping
// calcDrawVtx CALLS this function — it is the tested code, not a copy.
//
// Inputs (all VIEW space; velocity already scaled by the particle type's mExtension):
//   center[3]  — view-space particle centre (x,y,z)
//   vel[3]     — view-space velocity (x,y used; z ignored for the quad shape)
//   half       — half-size of the axis-aligned quad (= 1.414 * 0.5 * particleSize in the original)
//   stretch    — along-velocity stretch factor (TModelWaterManager::unk5D18)
// Output: out[4][3] — the 4 view-space quad corners (all share z = center z: a depth-planar billboard).
#ifndef SMS_WATER_BILLBOARD_H
#define SMS_WATER_BILLBOARD_H

#include <cmath>

// Magnitude of the xy velocity via the Gekko frsqrte + one Newton-Raphson step, exactly as the
// original ( dVar8 = 1/sqrt(vsq) estimate; mag = vsq*0.5*est*(3 - vsq*est*est) == sqrt(vsq) ).
// With an exact estimate this equals sqrtf(vsq); kept in this form to mirror the RE'd math.
inline float sb_water_xy_magnitude(float vsq) {
    float est = 1.0f / std::sqrt(vsq);
    return vsq * 0.5f * est * (3.0f - vsq * est * est);
}

inline void sb_water_billboard_corners(const float center[3], const float vel[3],
                                       float half, float stretch, float out[4][3]) {
    const float cx = center[0], cy = center[1], cz = center[2];
    const float vsq = vel[0] * vel[0] + vel[1] * vel[1];

    if (vsq <= 1.0f) {
        // Slow particle → axis-aligned square (half-size `half`), CCW from top-left.
        out[0][0] = cx - half; out[0][1] = cy + half;
        out[1][0] = cx + half; out[1][1] = cy + half;
        out[2][0] = cx + half; out[2][1] = cy - half;
        out[3][0] = cx - half; out[3][1] = cy - half;
    } else {
        // Fast particle → quad stretched along the view-space velocity, plus a `stretch` lead offset.
        const float mag = sb_water_xy_magnitude(vsq);
        const float s2  = (1.0f / mag) * half;
        const float fx  = vel[0] * s2;
        const float fy  = vel[1] * s2;
        out[0][0] = vel[0] * stretch + cx + fx; out[0][1] = vel[1] * stretch + cy + fy;
        out[1][0] = cx + fy;                    out[1][1] = cy - fx;
        out[2][0] = cx - fx - vel[0] * stretch; out[2][1] = cy - fy - vel[1] * stretch;
        out[3][0] = cx - fy;                    out[3][1] = cy + fx;
    }
    out[0][2] = out[1][2] = out[2][2] = out[3][2] = cz;
}

#endif // SMS_WATER_BILLBOARD_H
