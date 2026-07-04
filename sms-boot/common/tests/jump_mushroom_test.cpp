// jump_mushroom_test — spec-derived unit test for the TJumpMushroom::load port's
// serialized-record → collision-id decoder. Pure logic, Dolphin-free / no ROM.
//
// Spec (from disasm of 0x801f57a0):
//   the RE reads 4 bytes from the scene stream, treats the numeric BE u32 value as
//   `raw`, then passes `extsh(raw)` — i.e. sign-extended low 16 bits — to
//   TMapCollisionBase::setAllData(s16). Top 2 bytes are ignored padding.
//
// Regressions this catches:
//   • Taking the HIGH 16 bits instead of the low (would return the padding).
//   • Zero-extending instead of sign-extending (breaks negative ids like -1 sentinel).
//   • Using an unsigned type (silent sign flip on `setAllData`'s signed param).

#include "sms_boot_jumpmushroom.h"
#include <cstdio>
#include <cstdint>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
	std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

static void test_zero() {
	CHECK(sb::jump_mushroom_collision_id_from_serialized(0u) == 0, "0 → 0");
}
static void test_small_positive() {
	CHECK(sb::jump_mushroom_collision_id_from_serialized(0x00000007u) == 7, "7 → 7");
}
static void test_padding_ignored() {
	// Top 2 bytes are garbage — the decoder must ignore them.
	CHECK(sb::jump_mushroom_collision_id_from_serialized(0xDEAD0007u) == 7,
	      "0xDEAD0007 → 7 (padding ignored)");
	CHECK(sb::jump_mushroom_collision_id_from_serialized(0xFFFF0000u) == 0,
	      "0xFFFF0000 → 0 (top-half FFFF must not leak in)");
}
static void test_negative_sign_extended() {
	// s16 -1 arrives packed as 0x0000FFFF. If the port zero-extends we'd get +65535
	// (invalid for a signed collision-data id); sign-extend gives -1.
	CHECK(sb::jump_mushroom_collision_id_from_serialized(0x0000FFFFu) == -1,
	      "0x0000FFFF → -1 (sign extend)");
	CHECK(sb::jump_mushroom_collision_id_from_serialized(0x00008000u) == INT16_MIN,
	      "0x00008000 → INT16_MIN (sign extend on the sign bit)");
}
static void test_max_positive() {
	CHECK(sb::jump_mushroom_collision_id_from_serialized(0x00007FFFu) == 0x7FFF,
	      "0x00007FFF → INT16_MAX");
}

int main() {
	test_zero();
	test_small_positive();
	test_padding_ignored();
	test_negative_sign_extended();
	test_max_positive();
	if (g_fail) { std::fprintf(stderr, "jump_mushroom_test: %d FAILURE(S)\n", g_fail); return 1; }
	std::printf("jump_mushroom_test: all passed\n");
	return 0;
}
