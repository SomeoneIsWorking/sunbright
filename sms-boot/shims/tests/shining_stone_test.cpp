// shining_stone_test — spec-derived unit test for the TShiningStone port's sparkle-emit
// count. Pure logic, Dolphin-free / no ROM / no GPU / no particle manager.
//
// Spec (from scratch/decomp_next/801d07b4.c): the RE unrolls three sequential `if (N < unk74)`
// checks with N = 0, 1, 2. That is precisely `min(unk74, 3)` for non-negative unk74.

#include "sms_boot_shining_stone.h"
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

static void test_zero_emits_none() {
    CHECK(sb::shining_stone_particle_emit_count(0) == 0,
          "activation 0 → 0 emits (RE: no 0<0 hit)");
}

static void test_progression_1_2_3() {
    CHECK(sb::shining_stone_particle_emit_count(1) == 1, "activation 1 → 1 emit (0x143 only)");
    CHECK(sb::shining_stone_particle_emit_count(2) == 2, "activation 2 → 2 emits (0x143, 0x144)");
    CHECK(sb::shining_stone_particle_emit_count(3) == 3, "activation 3 → 3 emits (all three particle IDs)");
}

static void test_clamps_at_three() {
    // Above 3 → clamp (matches the RE's 3-check unroll: no 4th if exists).
    CHECK(sb::shining_stone_particle_emit_count(4)   == 3, "activation 4 → clamps to 3");
    CHECK(sb::shining_stone_particle_emit_count(100) == 3, "activation 100 → clamps to 3");
}

static void test_negative_defensive() {
    // The RE compares as signed int; a negative activation (shouldn't happen but sanity) → 0 emits.
    CHECK(sb::shining_stone_particle_emit_count(-1)   == 0, "activation -1 → 0");
    CHECK(sb::shining_stone_particle_emit_count(-100) == 0, "activation -100 → 0");
}

// Sensitivity: prove that a wrong bound would fail. If someone changes < to <= in the emit
// count, activation=0 would emit 1 (0 < 1 is true). Guard by asserting exact values.
static void test_boundary_sensitivity() {
    // If the port ever regresses to using `activation >= 0` (accepting zero as 1 emit), THIS
    // fails. Named because the RE's `0 < unk74` is strict less-than and off-by-one there is
    // exactly the "0 activation still sparkles" visible artifact.
    CHECK(sb::shining_stone_particle_emit_count(0) != 1,
          "sensitivity: activation 0 must NOT emit 1 (strict-less-than boundary)");
}

// ── load @0x801d0564 pure math: degrees → int-truncated s16 rotation units. ──
// SDA2 constant 0x43360b61 = 182.04444... = 65536/360, applied to each Vec3 rotation
// component. Spec-derived exact values:
//   0°   → 0
//   90°  → 90*182.04444 = 16384.0 → truncated 16384  (i.e. 0x4000, quarter-turn s16)
//   -90° → -16384
//   180° → 180*182.04444 = 32768.0 → truncated 32768 (out of s16 range; wraps to -32768
//                                                    when narrowed by MsMtxSetXYZRPH)
//   -180°→ -32768
//   45°  → 45*182.04444 = 8192.0 → 8192  (0x2000)
//   45.5°→ 45.5*182.04444 = 8283.0222... → truncated 8283 (NOT rounded 8283 also, but
//          we exercise a fractional to guarantee truncation semantics survive)
static void test_deg_to_rot_int_common_angles() {
    CHECK(sb::shining_stone_deg_to_rot_int(0.0f)    == 0,     "0deg → 0 units");
    CHECK(sb::shining_stone_deg_to_rot_int(90.0f)   == 16384, "90deg → 16384 (0x4000)");
    CHECK(sb::shining_stone_deg_to_rot_int(-90.0f)  == -16384,"-90deg → -16384");
    CHECK(sb::shining_stone_deg_to_rot_int(180.0f)  == 32768, "180deg → 32768 (int32; s16-narrow -> -32768)");
    CHECK(sb::shining_stone_deg_to_rot_int(-180.0f) == -32768,"-180deg → -32768");
    CHECK(sb::shining_stone_deg_to_rot_int(45.0f)   == 8192,  "45deg → 8192 (0x2000)");
}
static void test_deg_to_rot_int_truncation_not_round() {
    // 30.5 * 182.04444 = 5552.3554... → truncation gives 5552, rounding-to-nearest would
    // give the same value. Pick a boundary that distinguishes: 89.9997 * 182.04 =
    // 16383.945; trunc=16383, round=16384. This asserts we're NOT rounding.
    int r = sb::shining_stone_deg_to_rot_int(89.9997f);
    CHECK(r == 16383, "89.9997deg truncates to 16383 (rounding would give 16384)");
}
static void test_spoke_bmd_paths_table() {
    const char* const* p = sb::shining_stone_spoke_bmd_paths();
    CHECK(std::string("/scene/mapObj/ShiningStoneGreen.bmd") == p[0], "spoke[0] = Green");
    CHECK(std::string("/scene/mapObj/ShiningStoneBlue.bmd")  == p[1], "spoke[1] = Blue");
    CHECK(std::string("/scene/mapObj/ShiningStoneRed.bmd")   == p[2], "spoke[2] = Red");
    CHECK(std::string("/scene/mapObj/ShiningStoneWhite.bmd") == p[3], "spoke[3] = White");
}

int main() {
    test_zero_emits_none();
    test_progression_1_2_3();
    test_clamps_at_three();
    test_negative_defensive();
    test_boundary_sensitivity();
    test_deg_to_rot_int_common_angles();
    test_deg_to_rot_int_truncation_not_round();
    test_spoke_bmd_paths_table();
    if (g_fail) { std::fprintf(stderr, "shining_stone_test: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("shining_stone_test: all passed\n");
    return 0;
}
