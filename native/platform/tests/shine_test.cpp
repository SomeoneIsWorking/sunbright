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

	if (g_fail) { std::fprintf(stderr, "shine_test: %d FAILURE(S)\n", g_fail); return 1; }
	std::printf("shine_test: all passed\n");
	return 0;
}
