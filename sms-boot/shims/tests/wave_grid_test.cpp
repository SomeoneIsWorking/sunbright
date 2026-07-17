// wave_grid_test — spec-derived unit test for the TMapObjWave grid/wave port
// (decomp/sms/src/MoveBG/MapObjWave.cpp), the pure math extracted into
// native/render/sms_boot_wave_grid.h and CALLED BY the shipping load()/draw()/
// updateTime()/getWaveHeight() so a broken test flags a real regression.
//
// Ground truth comes from the RE (SDA2 constants + observed values captured under
// SUNBRIGHT_DBG_GXTEV / SUNBRIGHT_GX_ATTRIB on the settled title-screen frame):
//   • grid: extent=5200, step=200 → half=2600, inv_half=1/2600, strips=26,
//     verts-per-strip=52, total=1352 verts. The 1352 matches the oracle's
//     26-draws × 52-verts tev=2 SRCALPHA/SRCCLR pass3 composite exactly.
//   • fade: opaque at (xc=0,zc=0), 0.5 at edge-midpoint, 0 at the corner.
//   • phase advance: single subtract, wraps at 2π (not fmod).
//   • wave-height: two-axis sinusoid, kInv2Pi = 0.15915507f.

#include "sms_boot_wave_grid.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

static bool approx(float a, float b, float eps = 1e-4f) {
    float d = a - b; if (d < 0) d = -d;
    return d <= eps;
}

// ── grid_dims: the load()-computed constants ────────────────────────────────────
static void test_grid_dims_title_map() {
    // The title-screen (stage-15) constants — the ones oracle attribution proved
    // are the reflective sea's ground truth.
    const auto g = sb::wave::grid_dims(5200.0f, 200.0f);
    CHECK(approx(g.half_extent, 2600.0f), "half_extent = extent * 0.5");
    CHECK(approx(g.inv_half_extent, 1.0f / 2600.0f, 1e-9f), "inv_half_extent = 1/half");
    CHECK(g.strip_count == 26,          "strip_count = extent/step (title)");
    CHECK(g.verts_per_strip == 52,      "verts_per_strip = 2 * strip_count");
    CHECK(g.total_verts == 26 * 52,     "total_verts = 1352 (matches oracle sea composite)");
    CHECK(g.total_verts == 1352,        "total_verts literal check");
}

static void test_grid_dims_truncates() {
    // fexec-charset: extent/step truncation must be TOWARD ZERO (s32 cast), not round.
    // 5300/200 = 26.5 → truncate to 26 (not 27). Catches an accidental lroundf.
    const auto g = sb::wave::grid_dims(5300.0f, 200.0f);
    CHECK(g.strip_count == 26, "strip_count truncates 5300/200 → 26 (not 27)");
}

// ── fade_ratio: the per-vertex edge-fade alpha ratio ────────────────────────────
static void test_fade_centre_opaque() {
    // Mario-centred origin: fully opaque.
    CHECK(approx(sb::wave::fade_ratio(0.0f, 0.0f, 1.0f / 2600.0f), 1.0f),
          "(0,0) → ratio=1 (opaque)");
}

static void test_fade_edge_midpoint_half() {
    // At half the max extent along one axis: ratio = 1 - (1300/2600) = 0.5.
    CHECK(approx(sb::wave::fade_ratio(1300.0f, 0.0f, 1.0f / 2600.0f), 0.5f),
          "(1300, 0) → ratio=0.5");
    CHECK(approx(sb::wave::fade_ratio(0.0f, -1300.0f, 1.0f / 2600.0f), 0.5f),
          "(0, -1300) → ratio=0.5 (uses |zc|)");
}

static void test_fade_corner_clamps_zero() {
    // Past the corner: ratio would go negative, must clamp to 0.
    CHECK(approx(sb::wave::fade_ratio(3000.0f, 3000.0f, 1.0f / 2600.0f), 0.0f),
          "far past corner → ratio clamps to 0");
    CHECK(approx(sb::wave::fade_ratio(-2600.0f, 0.0f, 1.0f / 2600.0f), 0.0f),
          "exactly at -half → 0");
}

static void test_fade_uses_max_axis() {
    // The RE takes fmax(|xc|, |zc|), so the fade is DIAGONAL-symmetric.
    // (500, 1500) and (1500, 500) both fade by |max|=1500 → ratio = 1 - 1500/2600.
    const float inv = 1.0f / 2600.0f;
    const float expected = 1.0f - 1500.0f * inv;
    CHECK(approx(sb::wave::fade_ratio(500.0f, 1500.0f, inv), expected),
          "fade uses max(|xc|,|zc|), not xc alone (a)");
    CHECK(approx(sb::wave::fade_ratio(1500.0f, 500.0f, inv), expected),
          "fade uses max(|xc|,|zc|), not xc alone (b)");
}

static void test_fade_alpha_u8() {
    // fade_alpha wraps fade_ratio and multiplies by alpha_full: (0.5 * 255) → 127.
    // With mAlpha = 255 (the ctor default) and edge-midpoint fade, alpha ≈ 127.
    uint8_t a = sb::wave::fade_alpha(1300.0f, 0.0f, 1.0f / 2600.0f, 255.0f);
    CHECK(a == 127, "fade_alpha(1300,0) with alpha_full=255 → 127");
    a = sb::wave::fade_alpha(0.0f, 0.0f, 1.0f / 2600.0f, 255.0f);
    CHECK(a == 255, "fade_alpha at centre → 255 (opaque)");
}

// ── phase_advance: single-subtract 2π wrap ───────────────────────────────────────
static void test_phase_advance_simple() {
    CHECK(approx(sb::wave::phase_advance(0.0f, 0.02f), 0.02f),
          "phase 0 + 0.02 → 0.02");
    CHECK(approx(sb::wave::phase_advance(6.0f, 0.02f), 6.02f),
          "phase 6.0 + 0.02 → 6.02 (below 2π = 6.28318, no wrap)");
}

static void test_phase_advance_wraps_once() {
    // Just past 2π must wrap to a small positive value (NOT fmod — a single subtract).
    // 6.28 + 0.02 = 6.30 → wrap to 6.30 - 6.28318 ≈ 0.01682.
    float wrapped = sb::wave::phase_advance(6.28f, 0.02f);
    CHECK(wrapped >= 0.0f && wrapped < 0.05f,
          "phase 6.28 + 0.02 wraps into [0, 0.05)");
    CHECK(approx(wrapped, 6.30f - 6.28318f), "phase wrap subtracts exactly kTwoPi");
}

static void test_phase_no_double_wrap() {
    // The RE does a SINGLE subtract, not fmod. If the caller ever passes an insane
    // freq greater than 2π, the phase stays over 2π (that's faithful — the game
    // trusts freq << 2π). This test locks the semantics so a "helpful" fmod
    // rewrite is caught.
    float result = sb::wave::phase_advance(6.28318f, 20.0f);
    CHECK(result > 6.28318f, "single-subtract wrap: freq>2π stays over 2π (locked semantics)");
}

// ── wave_height_2d: the shared two-axis sinusoid ────────────────────────────────
static void test_height_at_origin_and_zero_phase() {
    // At (0, 0) with phase=0: sin(0)+sin(0) = 0.
    float y = sb::wave::wave_height_2d(0.0f, 0.0f, 30.0f, 0.02f, 0.0f, 25.0f, 0.03f, 0.0f);
    CHECK(approx(y, 0.0f), "height (0,0,phase=0) → 0");
}

static void test_height_reproducible_from_getWaveHeight() {
    // Reproduce the exact RE formula and cross-check the helper against a hand-
    // computed value. A wrong kInv2Pi or missed multiplication would trip this.
    const float kInv2Pi = 0.15915507f;
    const float x = 1000.0f, z = -500.0f;
    const float ax = 30.0f, fx = 0.02f, px = 0.5f;
    const float az = 25.0f, fz = 0.03f, pz = 0.1f;
    float expected = ax * std::sin(fx * (kInv2Pi * x) + px)
                   + az * std::sin(fz * (kInv2Pi * z) + pz);
    float got = sb::wave::wave_height_2d(x, z, ax, fx, px, az, fz, pz);
    CHECK(approx(got, expected), "wave_height_2d matches spec at (1000, -500)");
}

int main() {
    test_grid_dims_title_map();
    test_grid_dims_truncates();
    test_fade_centre_opaque();
    test_fade_edge_midpoint_half();
    test_fade_corner_clamps_zero();
    test_fade_uses_max_axis();
    test_fade_alpha_u8();
    test_phase_advance_simple();
    test_phase_advance_wraps_once();
    test_phase_no_double_wrap();
    test_height_at_origin_and_zero_phase();
    test_height_reproducible_from_getWaveHeight();
    if (g_fail) { std::fprintf(stderr, "wave_grid_test: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("wave_grid_test: all passed\n");
    return 0;
}
