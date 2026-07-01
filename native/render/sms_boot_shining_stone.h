// sms_boot_shining_stone.h — pure, Dolphin-free unit for the TShiningStone::perform port.
// The one testable pure piece is the "how many sparkle particles to emit given the current
// activation count" clamp: the RE unrolls three separate `if (N < unk74)` checks that add up
// to `min(unk74, 3)`. Extracted here so the shipping port + unit test share the same math.

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

}  // namespace sb
