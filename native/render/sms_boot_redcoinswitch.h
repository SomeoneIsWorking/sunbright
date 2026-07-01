// sms_boot_redcoinswitch.h — pure timer-scaling helper + spec constants for the
// TRedCoinSwitch::load port (@0x801c088c). 赤コインスイッチ ("red-coin switch") — a floor
// switch that, when hit, spawns 8 red coins with a countdown timer to collect them all.
//
// The stream stores a s32 duration; the RE clamps non-positive values to 1200 (the design
// default, 20 s @ 60 fps), else scales seconds→frames-ish via *10 (see gap note below).
// Extracting the piecewise into a helper catches two easy regressions:
//   * clamp direction reversed (>0 vs >=0): a zero seed would silently multiply-by-10 to 0.
//   * scale factor typo (60 vs 10): would silently 6× every mission's timer.
//
// NOTE — the *10 factor is what the DOL emits; whether it's 10-frame ticks (167 ms) or a
// different unit is not yet observed from the RE. Comment reflects the fact, not a guess.

#pragma once
#include <cstdint>

namespace sb {

constexpr std::int32_t kRedCoinSwitchDefaultTimer = 0x4B0;   // 1200 — the "no-value" default
constexpr std::int32_t kRedCoinSwitchTimerScale   = 10;      // scene s32 * 10 = frame budget

// TRedCoinSwitch::load's timer piecewise: non-positive → default; else scaled.
// Faithful to the RE (`cmpwi 0; bgt scale-path; else default`).
inline std::int32_t red_coin_switch_timer_from_serialized(std::int32_t serialized)
{
	if (serialized <= 0) {
		return kRedCoinSwitchDefaultTimer;
	}
	return serialized * kRedCoinSwitchTimerScale;
}

// Sentinel meaning "no shine belongs to this stage" — as returned by SMS_getShineIDofExStage
// for maps without a red-coin mission. The port skips the collected-check entirely in that
// case (any getShineFlag(0xFF) call would be an out-of-bounds bit index).
constexpr std::uint8_t kNoShineForStage = 0xFF;

}  // namespace sb
