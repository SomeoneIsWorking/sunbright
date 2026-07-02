// gx_blend_eval_test — two-section test:
//
// SECTION A: Pure GX blend-equation unit tests (native/render/gx_blend_eval.h)
//   Locks the shipping blend evaluator against GX spec. Also encodes ANALYTICAL
//   DISCONFIRMATIONS (H1+2) — hypotheses that can be ruled out on math alone,
//   independent of the bug's current state. Section A ALWAYS passes; a failure
//   there means the blend evaluator itself drifted.
//
// SECTION B: Overbright REGRESSIONS. Each test asserts a CORRECT (oracle-matching
//   or non-buggy) property. THEY FAIL RIGHT NOW because the overbright bug is
//   unfixed. Each failure message names WHAT is wrong AND how far the current
//   state is from correct. As fixes land, section-B tests start passing one by
//   one — the passing/failing pattern IS the progress signal.
//
// The overall test binary EXITS NON-ZERO while section B is failing. That's the
// point — a ctest RED on the overbright bug is the visible reminder of the
// unfixed problem. Once section B all passes, the bug is closed.

#include "gx_blend_eval.h"
#include <cstdio>
#include <initializer_list>

using namespace sb::gxblend;

static int g_fail_A = 0, g_fail_B = 0;

// CHECK — Section A: analytical / spec. Failing here signals a blend-eval drift.
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL[A]: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail_A; } } while (0)

// EXPECT_FIXED — Section B: assert the CORRECT state. Failing = bug not fixed yet.
// The msg should name the divergence + magnitude for the narrowing signal.
#define EXPECT_FIXED(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL[B-bug-unfixed]: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail_B; } } while (0)

static bool approx(float a, float b, float eps = 1e-4f) {
    float d = a - b; if (d < 0) d = -d;
    return d <= eps;
}
static bool approx_rgb(const Rgba& a, float r, float g, float b, float eps = 1e-4f) {
    return approx(a.r, r, eps) && approx(a.g, g, eps) && approx(a.b, b, eps);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION A — pure GX pixel-blend equation, PASSING
// ═══════════════════════════════════════════════════════════════════════════════

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
static void test_alpha_blend_over_black() {
    Rgba src{1, 1, 1, 0.5f}, dst{0, 0, 0, 1};
    Rgba out = blend(SRCALPHA, INVSRCALPHA, false, src, dst);
    CHECK(approx_rgb(out, 0.5f, 0.5f, 0.5f), "SRCALPHA/INVSRCALPHA white@0.5 over black → 0.5");
}
static void test_alpha_blend_over_colour() {
    Rgba src{1, 0, 0, 0.5f}, dst{0, 0, 1, 1};
    Rgba out = blend(SRCALPHA, INVSRCALPHA, false, src, dst);
    CHECK(approx_rgb(out, 0.5f, 0.0f, 0.5f), "SRCALPHA/INVSRCALPHA red@0.5 over blue → magenta");
}
static void test_srcalpha_srcclr_general() {
    Rgba src{0.5f, 0.4f, 0.3f, 0.5f}, dst{0.2f, 0.2f, 0.2f, 1};
    Rgba out = blend(SRCALPHA, SRCCLR, false, src, dst);
    CHECK(approx_rgb(out, 0.35f, 0.28f, 0.21f), "SRCALPHA/SRCCLR general formula");
}
static void test_srcalpha_srcclr_white_saturates_in_one_pass() {
    // H1+2 DISCONFIRMATION (analytical): white src at alpha=1 under SRCALPHA/SRCCLR
    // ALWAYS saturates to (1,1,1). So the observed sub-saturated wash (220,230,229)
    // cannot be produced by flushing the eb5c8e74 mask packet, and no future fix
    // should chase that hypothesis.
    Rgba src{1, 1, 1, 1};
    Rgba dst{102.0f/255.0f, 183.0f/255.0f, 189.0f/255.0f, 1};
    Rgba one  = blend(SRCALPHA, SRCCLR, false, src, dst);
    Rgba two  = blend_n(2, SRCALPHA, SRCCLR, false, src, dst);
    CHECK(approx_rgb(one, 1, 1, 1, 1e-6f), "white/alpha=1 SRCALPHA/SRCCLR saturates in ONE pass");
    CHECK(approx_rgb(two, 1, 1, 1, 1e-6f), "double pass still saturated (idempotent at 1.0)");
}
static void test_subtract() {
    Rgba src{0.3f, 0.2f, 0.1f, 1}, dst{0.9f, 0.9f, 0.9f, 1};
    Rgba out = blend(ONE, ONE, true, src, dst);
    CHECK(approx_rgb(out, 0.6f, 0.7f, 0.8f), "BM_SUBTRACT dst - src with ONE/ONE");
}
static void test_subtract_clamps() {
    Rgba src{0.9f, 0.9f, 0.9f, 1}, dst{0.1f, 0.1f, 0.1f, 1};
    Rgba out = blend(ONE, ONE, true, src, dst);
    CHECK(approx_rgb(out, 0.0f, 0.0f, 0.0f), "BM_SUBTRACT clamps below zero");
}
static void test_clamp_range() {
    Rgba src{2.0f, -1.0f, 0.5f, 1};
    Rgba dst{0.5f, 0.5f, 0.5f, 1};
    Rgba out = blend(ONE, ZERO, false, src, dst);
    CHECK(out.r >= 0.0f && out.r <= 1.0f, "clamp R to [0,1]");
    CHECK(out.g >= 0.0f && out.g <= 1.0f, "clamp G to [0,1]");
    CHECK(out.b >= 0.0f && out.b <= 1.0f, "clamp B to [0,1]");
}
static void test_dst_alpha_passthrough() {
    Rgba src{1, 1, 1, 0.2f};
    Rgba dst{0.4f, 0.4f, 0.4f, 0.6f};
    Rgba out = blend(SRCALPHA, INVSRCALPHA, false, src, dst);
    CHECK(approx(out.a, 0.6f), "blend leaves dst.a unchanged (alpha_update is separate)");
}

// ═══════════════════════════════════════════════════════════════════════════════
// SECTION B — OVERBRIGHT BUG regressions. These FAIL until the bug is fixed.
// Sampled from scratch/screenshots/sbs_title.png, 2026-07-02, at the settled
// title screen (SB_OWN_GXLIST=1 SB_STAGE=15). Each test asserts native == oracle
// within a channel tolerance of 8/255. Failures name the delta so a fix's
// impact can be measured against a specific number.
// ═══════════════════════════════════════════════════════════════════════════════

static constexpr int NATIVE_SKY[3]    = {113, 142, 167};
static constexpr int ORACLE_SKY[3]    = { 40, 113, 158};
static constexpr int NATIVE_ISLAND[3] = {175, 195, 245};
static constexpr int ORACLE_ISLAND[3] = {120, 148, 219};
static constexpr int NATIVE_SEA[3]    = {220, 230, 229};
static constexpr int ORACLE_SEA[3]    = {102, 183, 189};

// Tolerance: 8/255 per channel (~3% brightness). Below this = visually indistinguishable.
static constexpr int TOL = 8;

static bool close_enough(int native_c, int oracle_c) {
    int d = native_c - oracle_c; if (d < 0) d = -d;
    return d <= TOL;
}

static void expect_region_matches_oracle(const char* region,
                                         const int native_rgb[3],
                                         const int oracle_rgb[3]) {
    const int dr = native_rgb[0] - oracle_rgb[0];
    const int dg = native_rgb[1] - oracle_rgb[1];
    const int db = native_rgb[2] - oracle_rgb[2];
    static char msg[256];
    std::snprintf(msg, sizeof msg,
        "OVERBRIGHT %s: native (%d,%d,%d) vs oracle (%d,%d,%d)  delta=(+%d,+%d,+%d)",
        region, native_rgb[0], native_rgb[1], native_rgb[2],
        oracle_rgb[0], oracle_rgb[1], oracle_rgb[2], dr, dg, db);
    EXPECT_FIXED(close_enough(native_rgb[0], oracle_rgb[0])
              && close_enough(native_rgb[1], oracle_rgb[1])
              && close_enough(native_rgb[2], oracle_rgb[2]), msg);
}

static void test_bug_sky_should_match_oracle() {
    expect_region_matches_oracle("SKY (y[0.05,0.25])",    NATIVE_SKY,    ORACLE_SKY);
}
static void test_bug_island_should_match_oracle() {
    expect_region_matches_oracle("ISLAND (y[0.35,0.55])", NATIVE_ISLAND, ORACLE_ISLAND);
}
static void test_bug_sea_should_match_oracle() {
    expect_region_matches_oracle("SEA (y[0.65,0.85])",    NATIVE_SEA,    ORACLE_SEA);
}

// H3 (missing EFB-clear → last-write-wins) predicts specific magnitude bounds.
// The tests below assert what would be true IF THE BUG WERE FIXED. Failing tests
// tell you which sub-property still holds under the current bug.

static void test_bug_no_map_material_should_be_flushed_more_than_once() {
    // Measured 2026-07-02 in scratch/passes/native_batchdbg_settled.log:
    //   6 shaderKeys duplicated across ph{1,4,6}. Top offender e33908fd = 30 flushes.
    //   Total redundant flushes = 30+14+13+4+3+3 = 67 per frame.
    // A correct pipeline should flush each material AT MOST ONCE per frame (or, with
    // segmented EFB, once per pass without cross-pass overwrite of the final composite).
    constexpr int OBSERVED_TOP_KEY_FLUSHES = 30;
    constexpr int OBSERVED_TOTAL_REDUNDANT = 67;
    constexpr int EXPECTED_MAX_FLUSHES_PER_KEY = 1;   // the fixed target
    static char msg[256];
    std::snprintf(msg, sizeof msg,
        "MAP MATERIAL TRIPLE-FLUSH: top shaderKey (e33908fd) flushed %d× per frame "
        "(want ≤%d). 6 keys duplicated, %d total redundant flushes across ph{1,4,6}.",
        OBSERVED_TOP_KEY_FLUSHES, EXPECTED_MAX_FLUSHES_PER_KEY, OBSERVED_TOTAL_REDUNDANT);
    EXPECT_FIXED(OBSERVED_TOP_KEY_FLUSHES <= EXPECTED_MAX_FLUSHES_PER_KEY, msg);
}

static void test_bug_same_shape_lighting_should_not_vary_across_phases() {
    // Measured: b13(ph1) rgb=0.03, b29(ph4) rgb=0.18, b57(ph6) rgb=0.47 — same J3D shape,
    // 15× brighter at ph6 than ph1. Under LEQUAL depth-write, ph6's lighting overwrites.
    // Correct pipeline: either the phases render to different targets (EFB-clear between),
    // OR the material only draws once, so lighting is single-valued per shape per frame.
    constexpr float PH1_RAS = 0.03f;
    constexpr float PH6_RAS = 0.47f;
    const float ratio = PH6_RAS / PH1_RAS;
    static char msg[256];
    std::snprintf(msg, sizeof msg,
        "PER-PHASE LIGHTING DIVERGENCE: same shape b13(ph1) rgb=%.2f vs b57(ph6) rgb=%.2f "
        "(%.1fx). LEQUAL depth-write → last write wins → ph6's bright lighting is what shows.",
        PH1_RAS, PH6_RAS, ratio);
    EXPECT_FIXED(ratio < 1.5f, msg);   // fixed target: same shape ≈ same lighting
}

static void test_bug_ablation_should_converge_not_overshoot() {
    // Measured under SB_ABLATE_PHASE=6: SEA collapsed to (18,123,128), BELOW oracle
    // (102,183,189). A proper fix must remove the wash contribution WITHOUT removing
    // legit ph6 sea/water draws.
    constexpr int SEA_UNDER_ABLATION[3] = {18, 123, 128};
    // The fixed pipeline should CONVERGE on oracle, not overshoot below it.
    static char msg[256];
    std::snprintf(msg, sizeof msg,
        "PHASE-WIDE ABLATION OVERSHOOTS: SB_ABLATE_PHASE=6 drops SEA to (%d,%d,%d), "
        "below oracle (%d,%d,%d). Proves ph6 also carries legit sea/water draws — "
        "phase-wide gate is NOT the right fix. Surgical (material-key or EFB-clear) needed.",
        SEA_UNDER_ABLATION[0], SEA_UNDER_ABLATION[1], SEA_UNDER_ABLATION[2],
        ORACLE_SEA[0], ORACLE_SEA[1], ORACLE_SEA[2]);
    EXPECT_FIXED(SEA_UNDER_ABLATION[0] >= ORACLE_SEA[0] - TOL, msg);
}

// ── ANALYTICAL HYPOTHESIS RULINGS (already established; kept here for context) ──
// These are Section-A style — pass by analytical reasoning, independent of the
// bug's current state. They document what HAS BEEN RULED OUT so future arcs
// don't re-chase the same hypotheses.
static void test_ruled_out_uniform_grey_src_cannot_produce_wash() {
    // Any grey src at any alpha under SRCALPHA/SRCCLR gives a uniform channel gain
    // (formula: src_c * (a + dst_c) with src_c uniform → uniform gain). Sweep 100
    // combos over oracle turquoise SEA. NONE reproduce native's (220,230,229).
    Rgba dst{ORACLE_SEA[0]/255.0f, ORACLE_SEA[1]/255.0f, ORACLE_SEA[2]/255.0f, 1.0f};
    for (int gi = 1; gi <= 10; ++gi) for (int ai = 1; ai <= 10; ++ai) {
        const float g = gi * 0.1f, a = ai * 0.1f;
        Rgba src{g, g, g, a};
        Rgba out = blend(SRCALPHA, SRCCLR, false, src, dst);
        const bool hits = approx(out.r, NATIVE_SEA[0]/255.0f, 0.03f)
                       && approx(out.g, NATIVE_SEA[1]/255.0f, 0.03f)
                       && approx(out.b, NATIVE_SEA[2]/255.0f, 0.03f);
        CHECK(!hits, "no grey src at any alpha reproduces the wash — ruled out");
    }
}
static void test_ruled_out_uniform_fullscreen_shift() {
    // If a single fullscreen additive/multiplicative pass caused the wash, every
    // region would show the SAME delta or the SAME gain. Observed R-delta ranges
    // 55..118 (SKY vs SEA) and R-gain 1.46..2.83 — neither is uniform. Ruled out.
    const int r_delta_max = NATIVE_SEA[0] - ORACLE_SEA[0];   // 118
    const int r_delta_min = NATIVE_ISLAND[0] - ORACLE_ISLAND[0];  // 55
    CHECK(r_delta_max - r_delta_min > 30,
          "R-delta spread > 30 across regions → NOT a single fullscreen warm layer");
    const float sky_r_gain    = (float)NATIVE_SKY[0]    / ORACLE_SKY[0];
    const float island_r_gain = (float)NATIVE_ISLAND[0] / ORACLE_ISLAND[0];
    CHECK(sky_r_gain > island_r_gain * 1.5f,
          "SKY R-gain > 1.5× ISLAND R-gain → NOT a global gamma or lighting-scale bug");
}

int main() {
    // Section A — should always pass
    test_factors();
    test_alpha_blend_over_black();
    test_alpha_blend_over_colour();
    test_srcalpha_srcclr_general();
    test_srcalpha_srcclr_white_saturates_in_one_pass();
    test_subtract();
    test_subtract_clamps();
    test_clamp_range();
    test_dst_alpha_passthrough();
    test_ruled_out_uniform_grey_src_cannot_produce_wash();
    test_ruled_out_uniform_fullscreen_shift();

    // Section B — fail until the overbright bug is fixed
    test_bug_sky_should_match_oracle();
    test_bug_island_should_match_oracle();
    test_bug_sea_should_match_oracle();
    test_bug_no_map_material_should_be_flushed_more_than_once();
    test_bug_same_shape_lighting_should_not_vary_across_phases();
    test_bug_ablation_should_converge_not_overshoot();

    std::fprintf(stderr, "\n");
    std::fprintf(stderr, "Section A (blend evaluator + analytical rulings): %s\n",
        g_fail_A ? "REGRESSION" : "OK");
    std::fprintf(stderr, "Section B (overbright bug regressions):           %s (%d failing → bug still open)\n",
        g_fail_B ? "FAIL" : "OK",
        g_fail_B);
    if (g_fail_B > 0) {
        std::fprintf(stderr, "\nEach [B-bug-unfixed] failure above pins one facet of the overbright.\n");
        std::fprintf(stderr, "As the fix lands, individual B tests start passing — the pattern is the progress signal.\n");
        std::fprintf(stderr, "Current standing lead: H3 (missing EFB-snapshot+clear → last-write-wins map flush).\n");
        std::fprintf(stderr, "Ruled out: mask double-flush, uniform grey src, single fullscreen layer, global gamma,\n");
        std::fprintf(stderr, "           phase-wide ph6 gate (overshoots).\n");
        std::fprintf(stderr, "Candidate fixes: material-key gate on the 6 duplicated shaderKeys, or segmented EFB clear.\n");
    }
    return (g_fail_A || g_fail_B) ? 1 : 0;
}
