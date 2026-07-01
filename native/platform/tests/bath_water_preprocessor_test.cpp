// bath_water_preprocessor_test — spec-derived unit test for the pure dispatch predicate of
// the TBathWaterPreprocessor port. Pure, Dolphin-free / no ROM / no GPU / no manager /
// no renderer.
//
// Predicate spec (from scratch/decomp_bathwater/801aa5a8.c):
//   dispatch  iff  (flags & 8) && mgr && mgr->unk24 && mgr->unk30
// Each test hits ONE distinct exit of that decision so a wrong short-circuit shows up.

#include "sms_boot_bath_water.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

static void test_draw_bit_required() {
    // No DRAW bit → no dispatch, even with everything else present.
    CHECK(!sb::bath_water_preprocessor_should_dispatch(/*flags=*/0, true, true, true),
          "flags=0 (no DRAW) → no dispatch");
    // Just the anim bit (0x1) → no dispatch.
    CHECK(!sb::bath_water_preprocessor_should_dispatch(0x1, true, true, true),
          "flags=0x1 (ANIM only) → no dispatch — DRAW bit is 0x8, not 0x1");
    // DRAW bit set alone → dispatch (with everything else present).
    CHECK(sb::bath_water_preprocessor_should_dispatch(0x8, true, true, true),
          "flags=0x8 → dispatch");
    // DRAW combined with other bits → still dispatch.
    CHECK(sb::bath_water_preprocessor_should_dispatch(0x9, true, true, true),
          "flags=0x9 (ANIM|DRAW) → dispatch");
    CHECK(sb::bath_water_preprocessor_should_dispatch(0xFF, true, true, true),
          "flags=0xFF (all bits) → dispatch (DRAW bit is set)");
}

static void test_manager_null_short_circuits() {
    CHECK(!sb::bath_water_preprocessor_should_dispatch(0x8, /*mgr=*/false, true, true),
          "mgr null → no dispatch (would null-deref)");
}

static void test_bathtub_data_null_short_circuits() {
    CHECK(!sb::bath_water_preprocessor_should_dispatch(0x8, true, /*bathtub=*/false, true),
          "mgr->unk24 == 0 (no bathtub data) → no dispatch (matches RE `!= 0` guard)");
}

static void test_renderer_null_short_circuits() {
    CHECK(!sb::bath_water_preprocessor_should_dispatch(0x8, true, true, /*renderer=*/false),
          "mgr->unk30 (renderer) null → no dispatch (matches RE null guard)");
}

// Sensitivity: any single condition failing → no dispatch (the AND-chain is the whole spec).
static void test_all_four_gates_are_necessary() {
    // Turn each on individually while keeping the others OFF; none alone should dispatch.
    CHECK(!sb::bath_water_preprocessor_should_dispatch(0x8, false, false, false),
          "DRAW alone (nothing present) → no dispatch");
    CHECK(!sb::bath_water_preprocessor_should_dispatch(0x0, true, true, true),
          "everything present but no DRAW bit → no dispatch");
    // The only positive case:
    CHECK(sb::bath_water_preprocessor_should_dispatch(0x8, true, true, true),
          "all four gates true → dispatch");
}

int main() {
    test_draw_bit_required();
    test_manager_null_short_circuits();
    test_bathtub_data_null_short_circuits();
    test_renderer_null_short_circuits();
    test_all_four_gates_are_necessary();
    if (g_fail) { std::fprintf(stderr, "bath_water_preprocessor_test: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("bath_water_preprocessor_test: all passed\n");
    return 0;
}
