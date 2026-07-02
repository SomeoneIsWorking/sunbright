// sms_boot_lightcommon.h — pure spec-level helpers for the TLightCommon accessor
// ports (getLightPosition @0x80229ca0, getAmbColor @0x80229cec, getLightColor
// @0x80229d78). The header extends TLightCommon with the RE-derived field layout
// (mLocalPos[4], mLocalAmbColor[2], mLocalLightColor[4], the two override flags
// unk28/unk41, and idx offsets unk20/unk24). The impl files call into engine
// primitives (JDrama::TLightAry, GXGetLightColor); this header extracts the pure
// index-clamp + branch dispatch so the tests can pin them without dragging the
// engine along.

#pragma once
#include <cstdint>

namespace sb::light_common {

// The "local override" branch clamps idx to [0, N) by pinning any out-of-range
// input to 0 (not modular, not saturating — see the RE: `cmpwi r4, N; blt keep;
// li r4, 0`). N differs per accessor: 4 for getLightPosition/getLightColor,
// 2 for getAmbColor. Extracted so the tests can pin the boundary.
inline int clamp_local_idx(int idx, int limit)
{
    return (idx < limit) ? idx : 0;
}

// Return-branch decision: use the local override when the flag byte is nonzero.
// Both u8 flags (unk28 for ambient/color, unk41 for position) follow the same
// pattern. Extracted to catch a `!= 0` accidentally becoming `!= 1` or similar.
inline bool use_local(std::uint8_t flag)
{
    return flag != 0;
}

// Alpha-scaling used by getLightColor / getAmbColor's GROUP path. Faithful to
// the RE's `f32 * u8 -> fctiwz -> u8` sequence: multiply the u8 alpha by an
// f32 scale, TRUNCATE (not round) to int, cast to u8. Overflow wraps by u8
// truncation the same way the RE's `stb` does.
inline std::uint8_t scale_alpha_u8(std::uint8_t a, float scale)
{
    float f = static_cast<float>(a) * scale;
    int   i = static_cast<int>(f);  // C++ int-conversion truncates toward 0
    return static_cast<std::uint8_t>(i);
}

}  // namespace sb::light_common
