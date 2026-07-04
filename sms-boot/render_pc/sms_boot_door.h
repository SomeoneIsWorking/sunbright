// sms_boot_door.h — pure spec for the TDoor::load port (@0x801c24cc).
// The RE reads a serialized 32-bit int from the scene stream and treats it as a boolean:
// nonzero → the door is marked "locked" (whatever gameplay-gate that specific door uses:
// key, stage-flag, or story trigger). This header extracts the "nonzero → locked" mapping
// so a wrong operator (e.g. `!= 1` or `> 0`) would trip the unit test.

#pragma once
#include <cstdint>

namespace sb {

// The serialized-int-to-locked-bool predicate. Faithful to the RE: the compare is `!= 0`,
// so ANY nonzero value locks (0xFFFFFFFF, 1, or the sign-flipped -1 all lock; only literal
// zero unlocks).
inline bool door_locked_from_serialized(uint32_t serialized_value) {
    return serialized_value != 0;
}

}  // namespace sb
