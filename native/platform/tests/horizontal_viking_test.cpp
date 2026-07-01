// horizontal_viking_test — spec-derived unit test for the THorizontalViking::reset port
// (@0x801d6664). Pure logic, Dolphin-free / no ROM.
//
// Spec (from scratch/disasm.py 0x801d6664):
//   current  = target;         // stfs f0 → 0x144
//   velocity = 0.0f;            // stfs 0.0 → 0x148
//   state    = (target > 0.0f) ? 1 : 2;
//
// Regressions this catches:
//   * `> 0` → `>= 0`: zero target flips starting direction (state 2 → state 1).
//   * `> 0` → `< 0`: every swing starts mirrored.
//   * velocity accidentally left at input value instead of zeroed.

#include "sms_boot_horizontalviking.h"
#include <cstdio>
#include <cstdint>
#include <cmath>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
	std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

static void check_case(float target, uint16_t expected_state, const char* label) {
	sb::HorizontalVikingResetState r = sb::horizontal_viking_reset(target);
	CHECK(r.current  == target, label);
	CHECK(r.velocity == 0.0f,   label);
	CHECK(r.state    == expected_state, label);
}

int main() {
	check_case( 5.0f, 1, "positive target → state 1");
	check_case( 0.1f, 1, "small positive → state 1");
	check_case( 0.0f, 2, "zero target → state 2 (the > vs >= boundary)");
	check_case(-0.1f, 2, "small negative → state 2");
	check_case(-5.0f, 2, "negative target → state 2");

	// Explicit NaN/Inf are undefined per the RE but keep the port from crashing.
	sb::HorizontalVikingResetState r_inf = sb::horizontal_viking_reset(INFINITY);
	CHECK(std::isinf(r_inf.current),   "+inf preserved");
	CHECK(r_inf.velocity == 0.0f,      "+inf zero velocity");
	CHECK(r_inf.state    == 1,         "+inf → state 1");

	if (g_fail) { std::fprintf(stderr, "horizontal_viking_test: %d FAILURE(S)\n", g_fail); return 1; }
	std::printf("horizontal_viking_test: all passed\n");
	return 0;
}
