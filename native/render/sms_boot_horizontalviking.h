// sms_boot_horizontalviking.h — pure spec for the THorizontalViking::reset port
// (@0x801d6664). バイキング船 ("Viking ship") — the Pinna Park pirate-swing ride. The reset
// snapshots unk140 (the target swing angle) into unk144 (the current angle), zeros the
// angular velocity unk148, then latches mState based on the sign of the target: positive
// starts the swing going ONE direction (state=1), non-positive the other (state=2).
//
// Extracting this into a pure struct-returning helper catches two easy regressions:
//   * `>` reversed to `>=` — the sign-of-target 0.0 case flips its starting direction.
//   * `>` reversed to `<` — every swing starts the wrong way (game plays but is mirrored).

#pragma once
#include <cstdint>

namespace sb {

struct HorizontalVikingResetState {
	float current;
	float velocity;
	std::uint16_t state;
};

// Faithful to the RE: `fcmpu; ble else-branch; state=1; else: state=2`.
inline HorizontalVikingResetState horizontal_viking_reset(float target)
{
	HorizontalVikingResetState out;
	out.current  = target;   // stfs f0(0x140) → 0x144
	out.velocity = 0.0f;     // stfs 0.0    → 0x148
	out.state    = (target > 0.0f) ? std::uint16_t(1) : std::uint16_t(2);
	return out;
}

}  // namespace sb
