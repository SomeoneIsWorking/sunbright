// red_coin_switch_test — spec-derived unit test for the TRedCoinSwitch::load port's timer
// piecewise. Pure logic, Dolphin-free / no ROM.
//
// Spec (from disasm of 0x801c088c):
//   s32 raw = stream.readS32();  // BE-native
//   if (raw <= 0) mTimerDuration = 1200;
//   else          mTimerDuration = raw * 10;
//
// Regressions this catches:
//   * `<= 0` reversed to `< 0` → zero seed silently multiplies to zero (broken switch).
//   * scale factor typo (60/6/1 vs 10) → every stage's countdown mis-timed.
//   * signed/unsigned confusion → a "-1" sentinel would compute -10 instead of 1200.

#include "sms_boot_redcoinswitch.h"
#include <cstdio>
#include <cstdint>
#include <climits>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
	std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

int main() {
	// Non-positive → default.
	CHECK(sb::red_coin_switch_timer_from_serialized(0)      == 1200, "0 → 1200");
	CHECK(sb::red_coin_switch_timer_from_serialized(-1)     == 1200, "-1 → 1200");
	CHECK(sb::red_coin_switch_timer_from_serialized(INT_MIN) == 1200, "INT_MIN → 1200");

	// Positive → *10.
	CHECK(sb::red_coin_switch_timer_from_serialized(1)   == 10,     "1 → 10");
	CHECK(sb::red_coin_switch_timer_from_serialized(30)  == 300,    "30 → 300");
	CHECK(sb::red_coin_switch_timer_from_serialized(120) == 1200,   "120 → 1200");
	CHECK(sb::red_coin_switch_timer_from_serialized(999) == 9990,   "999 → 9990");

	// Constants pinned.
	CHECK(sb::kRedCoinSwitchDefaultTimer == 0x4B0, "default = 0x4B0");
	CHECK(sb::kRedCoinSwitchTimerScale   == 10,    "scale = 10");
	CHECK(sb::kNoShineForStage           == 0xFF,  "no-shine sentinel = 0xFF");

	if (g_fail) { std::fprintf(stderr, "red_coin_switch_test: %d FAILURE(S)\n", g_fail); return 1; }
	std::printf("red_coin_switch_test: all passed\n");
	return 0;
}
