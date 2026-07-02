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

// ── OVERBRIGHT-NARROWING: analyse the wash SIGNATURE (per-channel gain) ─────────
// Sampled pixel values at the settled title-screen lower band (scratch/screenshots/
// sbs_title.png, 2026-07-02, mean over y ∈ [0.65H, 0.85H], the sea band under the
// file-select UI):
static constexpr float ORACLE_RGB[3] = {102.0f/255.0f, 183.0f/255.0f, 189.0f/255.0f};
static constexpr float NATIVE_RGB[3] = {220.0f/255.0f, 230.0f/255.0f, 229.0f/255.0f};
// A grep of the native settled batchdbg dump (SB_OWN_GXLIST=1 SB_BATCH_DBG=-1) at
// 2026-07-02 found ZERO scene batches whose per-vertex mean colour is red-dominant
// (r > g+0.05 and r > b+0.05 and r > 0.2). So the wash driver is NOT one of the
// scene J3D batches' vertex-colour rasters. Candidates the remaining narrowing points
// at: (a) an IMM overlay batch (2D file-select UI / fader / HUD) with a warm tint,
// (b) a scene batch whose TEV combiner GENERATES a warm output from neutral vertex
// colour + a warm konst/tevreg (e.g. TEVREG0 sampled from a warm constant), or (c) an
// accumulated many-pass composite of near-neutral small contributions that add up to
// a warm net shift.

static void test_wash_gain_is_NON_UNIFORM() {
    // Multiplicative gain per channel: native / oracle.
    // r: 220/102 = 2.157   g: 230/183 = 1.257   b: 229/189 = 1.212
    // Green and blue barely move (~+20%); RED more than DOUBLES. This is the
    // signature of a CHROMATIC (colour-tinted) source, NOT an achromatic wash.
    const float gr = NATIVE_RGB[0] / ORACLE_RGB[0];
    const float gg = NATIVE_RGB[1] / ORACLE_RGB[1];
    const float gb = NATIVE_RGB[2] / ORACLE_RGB[2];
    CHECK(gr > 2.0f,  "red channel gain > 2.0 (strong shift)");
    CHECK(gg < 1.35f, "green channel gain < 1.35 (weak shift)");
    CHECK(gb < 1.30f, "blue channel gain  < 1.30 (weak shift)");
    // The signature: R shifts much more than G/B. Rules out any hypothesis where
    // the wash source is achromatic (grey / white / neutral tone) blended UNIFORMLY.
    CHECK(gr > gg * 1.5f && gr > gb * 1.5f,
          "red-channel gain >= 1.5x green/blue gains → source is CHROMATICALLY WARM");
}

static void test_no_uniform_grey_src_can_produce_the_wash() {
    // Any grey src (r == g == b) at ANY alpha via SRCALPHA/SRCCLR produces a
    // uniform shift ratio across channels (because the per-channel formula is
    // `src_c * (a + dst_c)` and src_c is the same for all channels). Sweep a
    // range of grey srcs + alphas — none should reproduce the observed shift
    // within tight tolerance. This is proof-by-exhaustion that the wash driver
    // is a COLOURED src, not a grey/white one.
    Rgba dst{ORACLE_RGB[0], ORACLE_RGB[1], ORACLE_RGB[2], 1.0f};
    for (int gi = 1; gi <= 10; ++gi) {          // grey levels 0.1 .. 1.0
        const float g = gi * 0.1f;
        for (int ai = 1; ai <= 10; ++ai) {      // alphas 0.1 .. 1.0
            const float a = ai * 0.1f;
            Rgba src{g, g, g, a};
            Rgba out = blend(SRCALPHA, SRCCLR, false, src, dst);
            const bool hits = approx(out.r, NATIVE_RGB[0], 0.03f)
                           && approx(out.g, NATIVE_RGB[1], 0.03f)
                           && approx(out.b, NATIVE_RGB[2], 0.03f);
            CHECK(!hits, "no grey src at any alpha can reproduce the wash under SRCALPHA/SRCCLR");
        }
    }
}

static void test_required_src_colour_is_warm() {
    // Given a SINGLE SRCALPHA/SRCCLR pass with src.a=1.0, solve for src.rgb that
    // maps ORACLE → NATIVE. Formula: out_c = src_c * (a + dst_c) → src_c = out_c / (a + dst_c).
    // For a=1.0 this yields src ≈ (0.616, 0.525, 0.516) — a WARM neutral (r > g > b).
    // Lock the derived colour so future analysis reads it here.
    const float a = 1.0f;
    const float sr = NATIVE_RGB[0] / (a + ORACLE_RGB[0]);
    const float sg = NATIVE_RGB[1] / (a + ORACLE_RGB[1]);
    const float sb = NATIVE_RGB[2] / (a + ORACLE_RGB[2]);
    CHECK(sr > sg && sg > sb,
          "solved src.rgb is r > g > b (warm-toned) — narrows the wash driver to a warm source "
          "(candidates: sun/glow, fader tint, or the file-select 'PUSH START' overlay)");
    CHECK(sr < 1.0f && sg < 1.0f && sb < 1.0f,
          "solved src.rgb all < 1.0 (physical colour) at src.a=1.0");
    // Verify the inverse: feeding the solved src back through the blend reproduces
    // the observed native pixel. If this check fails, the analytic formula drifted.
    Rgba src{sr, sg, sb, a};
    Rgba dst{ORACLE_RGB[0], ORACLE_RGB[1], ORACLE_RGB[2], 1.0f};
    Rgba out = blend(SRCALPHA, SRCCLR, false, src, dst);
    CHECK(approx(out.r, NATIVE_RGB[0], 1e-4f), "solved src produces observed native R");
    CHECK(approx(out.g, NATIVE_RGB[1], 1e-4f), "solved src produces observed native G");
    CHECK(approx(out.b, NATIVE_RGB[2], 1e-4f), "solved src produces observed native B");
}

static void test_low_alpha_would_require_impossible_src() {
    // At low src.a (e.g. 0.25), the required src.r would exceed 1.0 — impossible.
    // This bounds src.a from below: whatever draws the wash must have alpha >= ~0.35
    // to be physically realisable under this blend. Locks the alpha floor.
    const float a_bad = 0.25f;
    const float sr = NATIVE_RGB[0] / (a_bad + ORACLE_RGB[0]);   // 0.86/0.65 ≈ 1.32
    CHECK(sr > 1.0f,
          "src.a=0.25 requires unreachable src.r>1 → wash source's src.a must be higher");
    // Find the alpha floor where src.r == 1.0 exactly: src.r = 1 → a = out.r - dst.r.
    const float alpha_floor = NATIVE_RGB[0] - ORACLE_RGB[0];   // 0.86 - 0.4 = 0.46
    CHECK(alpha_floor > 0.4f && alpha_floor < 0.55f,
          "single-pass alpha floor ≈ 0.46 (below this, no valid src produces the wash)");
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
    test_wash_gain_is_NON_UNIFORM();
    test_no_uniform_grey_src_can_produce_the_wash();
    test_required_src_colour_is_warm();
    test_low_alpha_would_require_impossible_src();
    if (g_fail) { std::fprintf(stderr, "gx_blend_eval_test: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("gx_blend_eval_test: all passed\n");
    std::printf("  H1+2 (mask double-flush) DISCONFIRMED — predicts (1,1,1), observed (220,230,229).\n");
    std::printf("  Wash driver is a CHROMATICALLY WARM source (r>g>b), NOT a grey/white one.\n");
    std::printf("  Single-pass src.a floor ≈ 0.46; solved src @a=1.0 ≈ (0.616, 0.525, 0.516).\n");
    std::printf("  Candidates: sun glow (太陽遮蔽物グロー), lens flare (レンズフレア), fader tint.\n");
    return 0;
}
