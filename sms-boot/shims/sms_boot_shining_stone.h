// sms_boot_shining_stone.h — pure, Dolphin-free units for the TShiningStone port.
// The testable pure pieces are:
//   1. shining_stone_particle_emit_count(): the "how many sparkle particles to emit given the
//      current activation count" clamp used by perform (@0x801d07b4).
//   2. shining_stone_deg_to_rot_int(): the SDA2 - 0x280c constant (0x43360b61 = 182.04444...)
//      applied to each rotation-degree in load (@0x801d0564) with signed-int truncation,
//      matching the PPC fctiwz semantics before narrowing to s16 for MsMtxSetXYZRPH.
//   3. shining_stone_spoke_bmd_paths(): the fixed table of 4 spoke .bmd paths (Green/Blue/
//      Red/White) from the DOL string pool @0x80391b10 — used by load in the spoke loop.

#pragma once
#include <cstdint>

namespace sb {

// Given a shining-stone's activation count, return the number of sparkle particles to emit
// per spoke this frame. The RE @0x801d07b4 spells out three separate checks:
//   if (0 < unk74) emit(0x143)
//   if (1 < unk74) emit(0x144)
//   if (2 < unk74) emit(0x145)
// which is equivalent to a clamp `min(unk74, 3)` returning the emission count. Splitting the
// clamp into its own function names the intent (a "wake-up level" counter) and makes a wrong
// bound (e.g. `<=` instead of `<`) fail loudly in the test rather than silently over-emitting.
inline int shining_stone_particle_emit_count(int activation_count) {
    if (activation_count <= 0) return 0;
    if (activation_count > 3)  return 3;
    return activation_count;
}

// Degrees → PPC-integer-truncated s16 rotation units, matching the exact RE sequence used by
// TShiningStone::load @0x801d0564. The RE reads SDA2[-0x280c] = 0x43360b61 (= 65536.0f/360.0f
// as a f32), multiplies by the rotation-in-degrees f32, then `fctiwz` narrows to signed int
// (arg register). MsMtxSetXYZRPH's s16 param then takes the low 16 bits. The intermediate
// int32 truncation matters for |deg| > ~180 where a bare `static_cast<s16>(f)` is UB.
inline int32_t shining_stone_deg_to_rot_int(float deg) {
    // Bit-exact match to SDA2[-0x280c] = 0x43360b61 (= 65536.0f/360.0f, the standard
    // degrees→s16-rotation-units conversion). A literal like 182.04444f rounds down one
    // ULP (0x43360b60), which mis-truncates 90° to 16383 instead of 16384.
    return static_cast<int32_t>(deg * (65536.0f / 360.0f));
}

// The four .bmd paths for a stone's spoke ("ray") models, in the RE's fixed order. Ordering
// is spec-derived: pointer table at DOL 0x80391b10 → 0x80391a80 (Green), 0x80391aa4 (Blue),
// 0x80391ac8 (Red), 0x80391aec (White). A spoke is bound to its color slot by index (all four
// spokes on a single stone spin as one, so the order is presentational, not gameplay).
inline const char* const* shining_stone_spoke_bmd_paths() {
    static const char* const kPaths[4] = {
        "/scene/mapObj/ShiningStoneGreen.bmd",
        "/scene/mapObj/ShiningStoneBlue.bmd",
        "/scene/mapObj/ShiningStoneRed.bmd",
        "/scene/mapObj/ShiningStoneWhite.bmd",
    };
    return kPaths;
}

}  // namespace sb
