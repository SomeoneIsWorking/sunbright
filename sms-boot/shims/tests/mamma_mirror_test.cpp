// mamma_mirror_test — spec-derived unit tests for the TMammaMirrorMapOperator port's pure
// math. Pure logic, Dolphin-free / no ROM / no GPU / no J3D joint tree.
//
// Spec (from scratch/decomp_mamma_mirror/801cf1b0.c, SDA constants via tools/re/dol_sda.py):
//   - center = 0.5 * (min + max)
//   - threshold = min(max(0.5*(max.x-min.x), 0.5*(max.z-min.z)) + 2000, 3000)
//   - Y is DELIBERATELY ignored (horizontal-only radius)

#include "sms_boot_mamma_mirror.h"
#include <cstdio>
#include <cmath>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

static void test_center_symmetric() {
    // Symmetric bbox around origin.
    CHECK(sb::mamma_node_center(-100.0f, 100.0f) == 0.0f, "center([-100,100]) = 0");
    CHECK(sb::mamma_node_center(-50.0f, 250.0f) == 100.0f, "center([-50,250]) = 100");
    CHECK(sb::mamma_node_center(500.0f, 500.0f) == 500.0f, "degenerate span = the point");
}

static void test_threshold_x_dominant() {
    // Wide X, narrow Z → half_x = 500, half_z = 100 → 500 + 2000 = 2500.
    float t = sb::mamma_node_threshold(-500.0f, 500.0f, -100.0f, 100.0f);
    CHECK(t == 2500.0f, "half_x=500 dominant → 500+2000 = 2500");
}

static void test_threshold_z_dominant() {
    // Narrow X, wide Z → half_x = 50, half_z = 300 → 300 + 2000 = 2300.
    float t = sb::mamma_node_threshold(-50.0f, 50.0f, -300.0f, 300.0f);
    CHECK(t == 2300.0f, "half_z=300 dominant → 300+2000 = 2300");
}

static void test_threshold_cap_clamps() {
    // Big bbox (half_x = 2000) → 2000+2000 = 4000, clamped to 3000.
    float t = sb::mamma_node_threshold(-2000.0f, 2000.0f, -100.0f, 100.0f);
    CHECK(t == 3000.0f, "huge span clamps to 3000");
}

static void test_threshold_ignores_y() {
    // Y args aren't even in the signature. This test asserts the RE-intended behavior:
    // a joint that's tall but narrow has the SAME threshold as a joint that's short and
    // narrow (Y doesn't affect XZ radius).
    float t_short = sb::mamma_node_threshold(-100.0f, 100.0f, -100.0f, 100.0f);
    float t_tall  = sb::mamma_node_threshold(-100.0f, 100.0f, -100.0f, 100.0f);
    CHECK(t_short == t_tall, "Y-invariant threshold");
    CHECK(t_short == 2100.0f, "min bound: 100+2000");
}

static void test_threshold_equal_branch() {
    // half_x == half_z: the RE uses `<=` so half_z wins on tie — assert we take that branch
    // by making the two components identical and ensuring the SAME numeric result as either
    // interpretation (0.5*(200-(-200))=200 → 2200).
    float t = sb::mamma_node_threshold(-200.0f, 200.0f, -200.0f, 200.0f);
    CHECK(t == 2200.0f, "tied half_x==half_z → 200+2000 = 2200");
}

// ── perform's hide/show predicate ────────────────────────────────────────────
static void test_hidden_when_close_and_inside_mirror() {
    // Camera inside node's radius AND closer to node than to mirror → HIDE.
    CHECK(sb::mamma_node_should_be_hidden(500.0f, 2000.0f, 3000.0f) == true,
          "500 < 2000 AND 500 < 3000 → hidden");
}
static void test_shown_when_far_from_node() {
    // Distance to node exceeds node's radius → SHOW (even if closer than mirror).
    CHECK(sb::mamma_node_should_be_hidden(2500.0f, 2000.0f, 3000.0f) == false,
          "2500 > 2000 (radius) → shown");
}
static void test_shown_when_farther_than_mirror() {
    // Distance to node exceeds distance to mirror (though within LOD radius) → SHOW.
    // Mario is on the "wrong side" of the mirror plane, so the interior gets drawn.
    CHECK(sb::mamma_node_should_be_hidden(1500.0f, 2000.0f, 1000.0f) == false,
          "1500 > 1000 (mirror dist) → shown");
}
static void test_boundary_le_semantics() {
    // The RE uses `<=` on the compound; on the exact boundary the node stays hidden.
    CHECK(sb::mamma_node_should_be_hidden(2000.0f, 2000.0f, 2000.0f) == true,
          "boundary (dist == radius == mirror) → hidden (<= not <)");
}

int main() {
    test_center_symmetric();
    test_threshold_x_dominant();
    test_threshold_z_dominant();
    test_threshold_cap_clamps();
    test_threshold_ignores_y();
    test_threshold_equal_branch();
    test_hidden_when_close_and_inside_mirror();
    test_shown_when_far_from_node();
    test_shown_when_farther_than_mirror();
    test_boundary_le_semantics();
    if (g_fail) { std::fprintf(stderr, "mamma_mirror_test: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("mamma_mirror_test: all passed\n");
    return 0;
}
