// roll_block_test — spec-derived unit test for the TRollBlock::calcRootMatrix port
// (@0x801efdc4). Covers the pure Z-axis rotation-matrix build; the outer TRS+Concat
// dispatch is engine-primitive glue (MsMtxSetXYZRPH / PSMTXConcat / setBaseScale) with
// coverage elsewhere.
//
// Regressions this catches:
//   * cos/sin transposed (swap [0][0]↔[1][1] or [0][1]↔[1][0]) — the block would spin
//     around the wrong plane, mostly invisible in idle but painfully obvious in motion.
//   * -sin sign flipped (rotation reverses direction).
//   * Z-axis rotation misplaced (e.g. writing 1.0 to [0][0] instead of [2][2] would
//     build an X-axis or Y-axis rotation — the block would tumble instead of spin).
//   * Any nonzero garbage left in the [x][3] translation column.

#include "sms_boot_rollblock.h"
#include <cstdio>
#include <cmath>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

static bool feq(float a, float b) {
    return std::fabs(a - b) < 1e-6f;
}

int main() {
    float m[3][4];

    // Case 1: angle 0 (cos=1, sin=0) → identity in XY plane, unit Z.
    sb::rollblock_build_z_rot_mtx(1.0f, 0.0f, m);
    CHECK(feq(m[0][0], 1.0f) && feq(m[0][1], 0.0f), "angle 0 row0 xy");
    CHECK(feq(m[1][0], 0.0f) && feq(m[1][1], 1.0f), "angle 0 row1 xy");
    CHECK(feq(m[2][2], 1.0f),                        "angle 0 [2][2]=1");

    // Case 2: angle 90° (cos=0, sin=1) → +X maps to +Y (a positive-Z rotation).
    //   [ 0 -1  0 0]
    //   [ 1  0  0 0]
    //   [ 0  0  1 0]
    sb::rollblock_build_z_rot_mtx(0.0f, 1.0f, m);
    CHECK(feq(m[0][0], 0.0f) && feq(m[0][1], -1.0f), "angle 90 row0");
    CHECK(feq(m[1][0], 1.0f) && feq(m[1][1],  0.0f), "angle 90 row1");
    CHECK(feq(m[2][2], 1.0f),                         "angle 90 [2][2]=1");

    // Case 3: angle 180° (cos=-1, sin=0) → both X and Y flipped, Z preserved.
    sb::rollblock_build_z_rot_mtx(-1.0f, 0.0f, m);
    CHECK(feq(m[0][0], -1.0f) && feq(m[1][1], -1.0f), "angle 180 diag");
    CHECK(feq(m[0][1],  0.0f) && feq(m[1][0],  0.0f), "angle 180 off-diag");
    CHECK(feq(m[2][2],  1.0f),                          "angle 180 [2][2]=1");

    // Case 4: angle 45° (cos=sin=√2/2) — the -sin sign catches a sign-flip regression
    // that would silently reverse the roll direction and pass the 0°/180° cases.
    const float k = 0.70710678f;
    sb::rollblock_build_z_rot_mtx(k, k, m);
    CHECK(feq(m[0][0],  k) && feq(m[1][1], k),   "angle 45 diag=cos");
    CHECK(feq(m[0][1], -k),                       "angle 45 [0][1]=-sin (sign)");
    CHECK(feq(m[1][0],  k),                       "angle 45 [1][0]=+sin");

    // Case 5: translation column and all "zero" slots MUST stay zero — the RE's stfs
    // sequence writes 0.0 into every slot except the 5 populated ones; a stray uninit
    // read would ruin PSMTXConcat downstream.
    sb::rollblock_build_z_rot_mtx(0.5f, 0.3f, m);
    CHECK(m[0][2] == 0.0f && m[0][3] == 0.0f,     "row0 z/w zero");
    CHECK(m[1][2] == 0.0f && m[1][3] == 0.0f,     "row1 z/w zero");
    CHECK(m[2][0] == 0.0f && m[2][1] == 0.0f && m[2][3] == 0.0f,
          "row2 x/y/w zero");
    CHECK(m[2][2] == 1.0f,                         "row2 z pinned to 1.0");

    // Case 6: negative sin (roll going the other way) — verify -sin becomes +|sin|
    // in the [0][1] slot and -|sin| in the [1][0] slot. The port must NOT clamp.
    sb::rollblock_build_z_rot_mtx(0.6f, -0.8f, m);
    CHECK(feq(m[0][1],  0.8f),                     "-sin negates in [0][1]");
    CHECK(feq(m[1][0], -0.8f),                     "-sin preserves in [1][0]");

    if (g_fail) { std::fprintf(stderr, "roll_block_test: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("roll_block_test: all passed\n");
    return 0;
}
