// shine_test — spec-constant test for TShine::initMapObj (@0x801bcd70).
// Pure logic, Dolphin-free / no ROM.

#include "sms_boot_shine.h"
#include <cstdio>
#include <cstdint>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
	std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

int main() {
	CHECK(sb::shine_init::kUnk14C == 0x1e0, "unk14C = 0x1e0 (480)");
	CHECK(sb::shine_init::kUnk150 == 0x78,  "unk150 = 0x78 (120)");
	CHECK(sb::shine_init::kUnk1A4 == 0,     "unk1A4 = 0 (u8)");
	CHECK(sb::shine_init::kUnk1A8 == 0.0f,  "unk1A8 = 0.0f");
	CHECK(sb::shine_init::kUnk1AC == 0.0f,  "unk1AC = 0.0f");
	CHECK(sb::shine_init::kUnk1B0 == 0.0f,  "unk1B0 = 0.0f");
	CHECK(sb::shine_init::kUnk170 == 0xf0,  "unk170 = 0xf0 (240)");
	CHECK(sb::shine_init::kUnk174 == 0,     "unk174 = 0");
	CHECK(sb::shine_init::kUnk178 == 0xf0,  "unk178 = 0xf0 (240)");
	// Invariant: unk170 and unk178 share the same seed value.
	CHECK(sb::shine_init::kUnk170 == sb::shine_init::kUnk178,
	      "unk170 and unk178 seed to identical values");

	// --- TShine::loadAfter dispatch (@0x801bcd08) ---
	using E = sb::shine_load_after::Effect;
	CHECK(sb::shine_load_after::kWaitFrames == 0xf0, "loadAfter: wait-timer = 0xf0 (240)");
	CHECK(sb::shine_load_after::kWaitState  == 0x12, "loadAfter: wait-state = 0x12 (18)");
	CHECK(sb::shine_load_after::classify(0) == E::NoOp,               "loadAfter: unk154==0 → no-op");
	CHECK(sb::shine_load_after::classify(1) == E::MakeObjDead,        "loadAfter: unk154==1 → makeObjDead");
	CHECK(sb::shine_load_after::classify(2) == E::WaitTimerAndSet,    "loadAfter: unk154==2 → wait-timer branch");
	CHECK(sb::shine_load_after::classify(3) == E::NoOp,               "loadAfter: unk154>=3 → no-op");
	CHECK(sb::shine_load_after::classify(0xFFFFFFFFu) == E::NoOp,     "loadAfter: overflow-large unk154 → no-op");

	if (g_fail) { std::fprintf(stderr, "shine_test: %d FAILURE(S)\n", g_fail); return 1; }
	std::printf("shine_test: all passed\n");
	return 0;
}
