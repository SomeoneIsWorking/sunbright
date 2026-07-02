// gx_blend_eval_test — spec-derived unit test for the pure GX pixel-blend
// equation evaluator (native/render/gx_blend_eval.h) plus a discriminator
// test that locks a numerical finding about the file-select overbright:
//
//   With src=white / alpha=1.0, SRCALPHA/SRCCLR SATURATES to (1,1,1) in a
//   SINGLE pass regardless of dst. So the observed native lower-band wash
//   (~(220,230,229) over base turquoise (102,183,189)) CANNOT be produced by
//   flushing the eb5c8e74 mask packet once or twice — that would saturate to
//   full 255. Hypothesis 1+2 (mask double-flush explains the wash) is
//   NUMERICALLY DISCONFIRMED by this test. The wash driver is elsewhere; a
//   future overbright arc should not spend a session re-deriving this.

#include "gx_blend_eval.h"
#include <cstdio>

using namespace sb::gxblend;

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

static bool approx(float a, float b, float eps = 1e-4f) {
    float d = a - b; if (d < 0) d = -d;
    return d <= eps;
}
static bool approx_rgb(const Rgba& a, float r, float g, float b, float eps = 1e-4f) {
    return approx(a.r, r, eps) && approx(a.g, g, eps) && approx(a.b, b, eps);
}

// ── factor_value: verify each of the 8 factors evaluates to spec ────────────────
static void test_factors() {
    Rgba src{0.4f, 0.5f, 0.6f, 0.7f};
    Rgba dst{0.1f, 0.2f, 0.3f, 0.8f};
    CHECK(approx(factor_value(ZERO,        src, dst, 0), 0.0f),  "ZERO");
    CHECK(approx(factor_value(ONE,         src, dst, 0), 1.0f),  "ONE");
    CHECK(approx(factor_value(SRCCLR,      src, dst, 0), 0.4f),  "SRCCLR R");
    CHECK(approx(factor_value(SRCCLR,      src, dst, 1), 0.5f),  "SRCCLR G");
    CHECK(approx(factor_value(INVSRCCLR,   src, dst, 0), 0.6f),  "INVSRCCLR R = 1-src.r");
    CHECK(approx(factor_value(SRCALPHA,    src, dst, 0), 0.7f),  "SRCALPHA (channel-independent)");
    CHECK(approx(factor_value(INVSRCALPHA, src, dst, 0), 0.3f),  "INVSRCALPHA");
    CHECK(approx(factor_value(DSTALPHA,    src, dst, 0), 0.8f),  "DSTALPHA");
    CHECK(approx(factor_value(INVDSTALPHA, src, dst, 0), 0.2f),  "INVDSTALPHA");
}

// ── SRCALPHA / INVSRCALPHA: the standard alpha blend ─────────────────────────────
static void test_alpha_blend_over_black() {
    // src white at 50% alpha over black → mid-grey.
    Rgba src{1, 1, 1, 0.5f}, dst{0, 0, 0, 1};
    Rgba out = blend(SRCALPHA, INVSRCALPHA, false, src, dst);
    CHECK(approx_rgb(out, 0.5f, 0.5f, 0.5f), "SRCALPHA/INVSRCALPHA white@0.5 over black → 0.5");
}
static void test_alpha_blend_over_colour() {
    // src=red at 50% alpha over dst=blue → half red + half blue.
    Rgba src{1, 0, 0, 0.5f}, dst{0, 0, 1, 1};
    Rgba out = blend(SRCALPHA, INVSRCALPHA, false, src, dst);
    CHECK(approx_rgb(out, 0.5f, 0.0f, 0.5f), "SRCALPHA/INVSRCALPHA red@0.5 over blue → magenta");
}

// ── SRCALPHA / SRCCLR: the "additive-ish, brightening" combo the mask packet uses ─
static void test_srcalpha_srcclr_general() {
    // out = src * srcAlpha + dst * srcColour  (per channel)
    // With src=(0.5, 0.4, 0.3) alpha=0.5 and dst=(0.2, 0.2, 0.2):
    //   r = 0.5*0.5 + 0.2*0.5 = 0.35
    //   g = 0.4*0.5 + 0.2*0.4 = 0.28
    //   b = 0.3*0.5 + 0.2*0.3 = 0.21
    Rgba src{0.5f, 0.4f, 0.3f, 0.5f}, dst{0.2f, 0.2f, 0.2f, 1};
    Rgba out = blend(SRCALPHA, SRCCLR, false, src, dst);
    CHECK(approx_rgb(out, 0.35f, 0.28f, 0.21f), "SRCALPHA/SRCCLR general formula");
}

// ── The DISCRIMINATOR: SRCALPHA/SRCCLR with src=white/alpha=1 saturates. ────────
// This is the numerical proof that "hypothesis 1+2" (the eb5c8e74 mask flushed
// once or twice explains the observed native wash) is FALSE. Locking this
// invariant here so a future overbright arc reads the answer instead of
// re-deriving it.
static void test_srcalpha_srcclr_white_saturates_in_one_pass() {
    // Native's b12 batchtev: rgb=1.00,1.00,1.00 a=1.00[1.00,1.00] — src=white/alpha=1.
    // Dst = the observed oracle turquoise sea (102, 183, 189) / 255 = (0.4, 0.717, 0.741).
    // Formula: out = 1.0 * 1.0 + dst * 1.0 = 1.0 + dst → CLAMPS to 1.0 on every channel.
    Rgba src{1.0f, 1.0f, 1.0f, 1.0f};
    Rgba dst{102.0f/255.0f, 183.0f/255.0f, 189.0f/255.0f, 1.0f};
    Rgba one  = blend(SRCALPHA, SRCCLR, false, src, dst);
    CHECK(approx_rgb(one, 1.0f, 1.0f, 1.0f, 1e-6f),
          "single pass of mask packet over turquoise → (1,1,1) saturation");
    // Double-pass (b12 then b72) — still saturated (idempotent once you hit 1.0).
    Rgba two  = blend_n(2, SRCALPHA, SRCCLR, false, src, dst);
    CHECK(approx_rgb(two, 1.0f, 1.0f, 1.0f, 1e-6f),
          "double pass of mask packet over turquoise → still (1,1,1)");
}
static void test_observed_wash_is_NOT_mask_double_flush() {
    // Observed native lower band from scratch/screenshots/sbs_title.png sampled 2026-07-02:
    // (220, 230, 229) — brightened but NOT fully saturated. If hypothesis 1+2 were the
    // driver, the pixel would be (255, 255, 255). Assert the mismatch so the test file
    // documents the disconfirmation, not just the arithmetic.
    Rgba src{1.0f, 1.0f, 1.0f, 1.0f};                      // b12/b72 tevreg-out white
    Rgba dst{102.0f/255.0f, 183.0f/255.0f, 189.0f/255.0f, 1.0f};   // oracle turquoise base
    Rgba predicted_double_flush = blend_n(2, SRCALPHA, SRCCLR, false, src, dst);
    const float observed_native[3] = {220.0f/255.0f, 230.0f/255.0f, 229.0f/255.0f};
    const bool matches =
           approx(predicted_double_flush.r, observed_native[0], 0.05f)
        && approx(predicted_double_flush.g, observed_native[1], 0.05f)
        && approx(predicted_double_flush.b, observed_native[2], 0.05f);
    CHECK(!matches,
          "H1+2 disconfirmed: mask double-flush prediction (1,1,1) does NOT match observed wash (220,230,229)");
}

// ── BM_SUBTRACT: the dst - src path (used by some GX draws) ──────────────────────
static void test_subtract() {
    // out = dst * df - src * sf  → dst - src when sf=ONE, df=ONE.
    Rgba src{0.3f, 0.2f, 0.1f, 1}, dst{0.9f, 0.9f, 0.9f, 1};
    Rgba out = blend(ONE, ONE, true, src, dst);
    CHECK(approx_rgb(out, 0.6f, 0.7f, 0.8f), "BM_SUBTRACT dst - src with ONE/ONE");
}
static void test_subtract_clamps_below_zero() {
    // Would produce negative → clamp to 0.
    Rgba src{0.9f, 0.9f, 0.9f, 1}, dst{0.1f, 0.1f, 0.1f, 1};
    Rgba out = blend(ONE, ONE, true, src, dst);
    CHECK(approx_rgb(out, 0.0f, 0.0f, 0.0f), "BM_SUBTRACT clamps below zero");
}

// ── Clamping invariant: any legal blend clamps to [0, 1] on all channels ────────
static void test_clamp_range() {
    Rgba src{2.0f, -1.0f, 0.5f, 1};   // pathological — factor_value doesn't clamp inputs
    Rgba dst{0.5f, 0.5f, 0.5f, 1};
    Rgba out = blend(ONE, ZERO, false, src, dst);
    CHECK(out.r >= 0.0f && out.r <= 1.0f, "clamp R to [0,1]");
    CHECK(out.g >= 0.0f && out.g <= 1.0f, "clamp G to [0,1]");
    CHECK(out.b >= 0.0f && out.b <= 1.0f, "clamp B to [0,1]");
}

// ── dst alpha passthrough — matches the aU=0 / cU=1 common case ─────────────────
static void test_dst_alpha_passthrough() {
    Rgba src{1, 1, 1, 0.2f};
    Rgba dst{0.4f, 0.4f, 0.4f, 0.6f};
    Rgba out = blend(SRCALPHA, INVSRCALPHA, false, src, dst);
    CHECK(approx(out.a, 0.6f), "blend leaves dst.a unchanged (alpha_update is separate)");
}

int main() {
    test_factors();
    test_alpha_blend_over_black();
    test_alpha_blend_over_colour();
    test_srcalpha_srcclr_general();
    test_srcalpha_srcclr_white_saturates_in_one_pass();
    test_observed_wash_is_NOT_mask_double_flush();
    test_subtract();
    test_subtract_clamps_below_zero();
    test_clamp_range();
    test_dst_alpha_passthrough();
    if (g_fail) { std::fprintf(stderr, "gx_blend_eval_test: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("gx_blend_eval_test: all passed (H1+2 disconfirmed: mask double-flush cannot explain the observed wash)\n");
    return 0;
}
