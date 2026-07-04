// water_billboard_test — unit tests for the pure port of TModelWaterManager::calcDrawVtx's
// per-particle quad math (native/render/sms_water_billboard.h), reverse-engineered from
// GMSE01 calcDrawVtx @0x8027e5f4. Asserts HAND-DERIVED corners for both branches (slow →
// axis-aligned square; fast → velocity-stretched quad). No Dolphin, no GPU, no ROM.

#include "sms_water_billboard.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

static bool feq(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
static bool corner_eq(const float c[3], float x, float y, float z) {
    return feq(c[0], x) && feq(c[1], y) && feq(c[2], z);
}

// ── 1. Slow particle (vsq <= 1) → axis-aligned square of half-size `half`, all corners at center z.
static void test_axis_aligned() {
    const float center[3] = { 10.f, 20.f, -5.f };
    const float vel[3]    = { 0.f, 0.f, 0.f };   // vsq = 0 <= 1
    float q[4][3];
    sb_water_billboard_corners(center, vel, /*half=*/2.f, /*stretch=*/0.5f, q);
    CHECK(corner_eq(q[0], 8.f, 22.f, -5.f),  "axis c0 = (cx-h, cy+h, cz)");
    CHECK(corner_eq(q[1], 12.f, 22.f, -5.f), "axis c1 = (cx+h, cy+h, cz)");
    CHECK(corner_eq(q[2], 12.f, 18.f, -5.f), "axis c2 = (cx+h, cy-h, cz)");
    CHECK(corner_eq(q[3], 8.f, 18.f, -5.f),  "axis c3 = (cx-h, cy-h, cz)");
}

// A velocity just under the threshold still takes the axis-aligned branch (vsq == 1 is <=).
static void test_axis_aligned_boundary() {
    const float center[3] = { 0.f, 0.f, -1.f };
    const float vel[3]    = { 1.f, 0.f, 0.f };   // vsq = 1 → axis-aligned (<=)
    float q[4][3];
    sb_water_billboard_corners(center, vel, 1.f, 0.5f, q);
    CHECK(corner_eq(q[0], -1.f, 1.f, -1.f), "boundary vsq==1 stays axis-aligned");
}

// ── 2. Fast particle (vsq > 1) → quad stretched along the velocity + `stretch` lead offset.
// center=(0,0,-3), vel=(3,4,0) → vsq=25, mag=5, s2=half/mag=1/5=0.2, fx=0.6, fy=0.8, stretch=0.5.
//   c0 = (vx*st + cx + fx, vy*st + cy + fy) = (1.5+0.6, 2.0+0.8) = (2.1, 2.8)
//   c1 = (cx + fy, cy - fx)                 = (0.8, -0.6)
//   c2 = (cx - fx - vx*st, cy - fy - vy*st) = (-0.6-1.5, -0.8-2.0) = (-2.1, -2.8)
//   c3 = (cx - fy, cy + fx)                 = (-0.8, 0.6)
static void test_stretched() {
    const float center[3] = { 0.f, 0.f, -3.f };
    const float vel[3]    = { 3.f, 4.f, 0.f };   // vsq = 25 > 1
    float q[4][3];
    sb_water_billboard_corners(center, vel, /*half=*/1.f, /*stretch=*/0.5f, q);
    CHECK(corner_eq(q[0], 2.1f, 2.8f, -3.f),   "stretch c0 = vel*st + center + f");
    CHECK(corner_eq(q[1], 0.8f, -0.6f, -3.f),  "stretch c1 = (cx+fy, cy-fx)");
    CHECK(corner_eq(q[2], -2.1f, -2.8f, -3.f), "stretch c2 = center - f - vel*st");
    CHECK(corner_eq(q[3], -0.8f, 0.6f, -3.f),  "stretch c3 = (cx-fy, cy+fx)");
}

// The frsqrte + Newton magnitude helper must equal sqrt to ~float precision.
static void test_magnitude() {
    CHECK(feq(sb_water_xy_magnitude(25.f), 5.f),   "mag(25) == 5");
    CHECK(feq(sb_water_xy_magnitude(2.f), std::sqrt(2.f)), "mag(2) == sqrt(2)");
    CHECK(feq(sb_water_xy_magnitude(100.f), 10.f), "mag(100) == 10");
}

int main() {
    test_axis_aligned();
    test_axis_aligned_boundary();
    test_stretched();
    test_magnitude();
    if (g_fail) { std::fprintf(stderr, "water_billboard_test: %d FAILED\n", g_fail); return 1; }
    std::printf("water_billboard_test: all passed\n");
    return 0;
}
