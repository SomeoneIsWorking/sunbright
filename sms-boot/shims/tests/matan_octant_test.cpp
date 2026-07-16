// matan_octant_test — close-test for the matan (octant-reduced atan2) port in
// reference/sms/src/MarioUtil/MathUtil.cpp (retail @0x8022ae08).
//
// NAMED defect (2026-07-16): a prior "OOB fix" rewrite of matan used the wrong
// octant offset in the (param_1<0, param_2>=0, |param_1|>=param_2) branch —
// `atan(p2/|p1|) + 0x8000` where retail emits `0x8000 - atan(p2/|p1|)` — and
// the symmetric -Y octants. That REFLECTS the returned angle, so
// CLBRevisionLookatByAngleX (CrossToPolar -> clamp pitch -> PolarToCross)
// mirrored the camera's look-target X around the eye: the file-select camera
// settled looking the wrong way (OPTIONS sign pushed off-screen).
//
// This test mirrors the SHIPPING branch structure (with the atan table modeled
// as std::atan) and asserts atan2 parity across all eight octants. The exact
// failing octant (matan(-994.9, 53.5), the settled file-select yaw) is checked
// explicitly. RED against the old branch offsets, GREEN against the retail-
// faithful ones. Pure logic, no ROM/GPU.

#include <cmath>
#include <cstdio>
#include <cstdint>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

// GetAtanTable(a,b) = atan(b/a) as an unsigned s16 angle, valid for 0<=b<=a.
static uint16_t atan_table(float a, float b) {
    if (a == 0.0f) return 0;
    double ang = std::atan2((double)b, (double)a);          // b/a, both >=0, in [0, 45deg]
    return (uint16_t)std::lround(ang / (2.0 * M_PI) * 65536.0);
}

// Faithful matan (must stay identical to MathUtil.cpp::matan's branch layout).
static int16_t matan_port(float p1, float p2) {
    if (p2 < 0.0f) {
        p2 = -p2;
        if (p1 >= 0.0f) {
            if (p2 <= p1) return (int16_t)(-(int)atan_table(p1, p2));
            else          return (int16_t)(0xc000 + atan_table(p2, p1));
        } else {
            p1 = -p1;
            if (p1 < p2)  return (int16_t)(0xc000 - atan_table(p2, p1));
            else          return (int16_t)(0x8000 + atan_table(p1, p2));
        }
    } else {
        if (p1 < 0.0f) {
            p1 = -p1;
            if (p2 <= p1) return (int16_t)(0x8000 - atan_table(p1, p2));
            else          return (int16_t)(0x4000 + atan_table(p2, p1));
        } else {
            if (p1 < p2)  return (int16_t)(0x4000 - atan_table(p2, p1));
            else          return (int16_t)atan_table(p1, p2);
        }
    }
}

// Reference: matan(a,b) is the angle of the vector (a,b) == atan2(b,a).
static int16_t ref_atan2(float p1, float p2) {
    double ang = std::atan2((double)p2, (double)p1);
    long s = std::lround(ang / (2.0 * M_PI) * 65536.0) & 0xFFFF;
    return (int16_t)(s < 0x8000 ? s : s - 0x10000);
}

static int adiff(int16_t a, int16_t b) {
    int d = ((int)a - (int)b + 32768) % 65536 - 32768;
    return d < 0 ? -d : d;
}

int main() {
    // The exact settled file-select yaw that was mirrored: dz=-994.9, dx=53.5.
    // Correct is +176.9deg (second quadrant, sin>0). The bug returned -176.9deg
    // (sin<0), which flipped target.x from eye+53.5 to eye-53.5.
    int16_t got = matan_port(-994.9f, 53.5f);
    int16_t want = ref_atan2(-994.9f, 53.5f);
    CHECK(adiff(got, want) <= 1, "matan(-994.9,53.5) yaw (settled file-select camera)");
    CHECK(got > 0, "matan(-994.9,53.5) must be POSITIVE (looking +X of forward), not mirrored");

    // Full sweep: every octant, atan2 parity. RED on any reflected branch.
    int maxerr = 0;
    for (int i = 0; i < 3600; ++i) {
        double th = i * (2.0 * M_PI / 3600.0);
        float p1 = (float)(1000.0 * std::cos(th));
        float p2 = (float)(1000.0 * std::sin(th));
        int e = adiff(matan_port(p1, p2), ref_atan2(p1, p2));
        if (e > maxerr) maxerr = e;
    }
    CHECK(maxerr <= 2, "matan matches atan2 across all octants (<=2 s16 units)");

    // Axis cases.
    CHECK(adiff(matan_port(1000.f, 0.f), 0) <= 1, "matan(+x,0) = 0");
    CHECK(adiff(matan_port(0.f, 1000.f), 0x4000) <= 1, "matan(0,+y) = 90deg");
    CHECK(adiff(matan_port(-1000.f, 0.f), (int16_t)0x8000) <= 1, "matan(-x,0) = 180deg");
    CHECK(adiff(matan_port(0.f, -1000.f), (int16_t)0xC000) <= 1, "matan(0,-y) = -90deg");

    if (g_fail == 0) std::printf("matan_octant_test: OK\n");
    return g_fail ? 1 : 0;
}
