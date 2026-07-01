// shining_stone_test — spec-derived unit test for the TShiningStone port's sparkle-emit
// count. Pure logic, Dolphin-free / no ROM / no GPU / no particle manager.
//
// Spec (from scratch/decomp_next/801d07b4.c): the RE unrolls three sequential `if (N < unk74)`
// checks with N = 0, 1, 2. That is precisely `min(unk74, 3)` for non-negative unk74.

#include "sms_boot_shining_stone.h"
#include <cstdio>

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

int main() {
    test_zero_emits_none();
    test_progression_1_2_3();
    test_clamps_at_three();
    test_negative_defensive();
    test_boundary_sensitivity();
    if (g_fail) { std::fprintf(stderr, "shining_stone_test: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("shining_stone_test: all passed\n");
    return 0;
}
