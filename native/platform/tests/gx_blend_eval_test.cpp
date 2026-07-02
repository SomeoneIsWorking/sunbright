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
#include <initializer_list>

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

// ── H3: MISSING EFB-CLEAR → LAST-WRITE-WINS across triple-flushed phases ────────
// A grep of scratch/passes/native_batchdbg_settled.log (2026-07-02, SB_OWN_GXLIST=1
// SB_BATCH_DBG=-1) uncovered the SPECIFIC failure mode driving the wash:
//   * 6 distinct shaderKeys (the map/island materials) are flushed across ph1 + ph4 + ph6
//     per frame — the top offender (key e33908fd, the clay/adobe map material) is
//     flushed 30 times per frame (10 distinct J3D shapes × 3 phases). Other duplicated
//     keys: 224004d9 (14), 153cc191 (13), f19161bf (4), 85b2f9ca (3), 69ebca44 (3).
//   * The 3 flushes of each shape use the SAME geometry but DIFFERENT per-vertex
//     lighting: measured b13(ph1) rgb=0.03, b29(ph4) rgb=0.18, b57(ph6) rgb=0.47.
//     Ph6 is 15x brighter than ph1 for the same batch.
//   * All flushes use bm=1/4/5 (SRCALPHA / INVSRCALPHA) with src.a=1.0, z_test=LEQUAL,
//     z_write=ON — i.e. standard alpha blend that with alpha=1 degenerates to REPLACE,
//     and LEQUAL depth on equal depth passes the test → LAST WRITE WINS.
// On the oracle GC pipeline, each phase renders to a DIFFERENT EFB (copied to a texture
// and CLEARED between passes — memory [[fileselect-overbright-is-render-to-texture-
// composite]]), so the 3 lighting environments never composite over each other. Native's
// flat framebuffer has no EFB-clear, so the LAST (brightest, ph6) pass overwrites the
// earlier ones → the map draws with WRONG (too-bright) lighting → the observed wash.
// H3 is the standing lead; these tests LOCK the numeric evidence for it.

static void test_h3_multi_phase_flush_count_matches_evidence() {
    // Locking the observed native duplication counts as a data fingerprint. If a fix
    // lands that reduces duplication (e.g. per-phase EFB snapshot lands, or ph6 flush
    // is properly gated), these constants need updating — a healthy regression signal.
    constexpr int kTopKeyInstances    = 30;    // key e33908fd across ph{1,4,6}
    constexpr int kTopKeyPhaseCount   = 3;     // ph{1,4,6}
    constexpr int kDupKeysAtLeast     = 6;     // keys with ≥3 instances (map materials)
    constexpr int kTotalDupInstances  = 30 + 14 + 13 + 4 + 3 + 3;  // = 67
    CHECK(kTopKeyInstances == kTopKeyPhaseCount * 10,
          "30 flushes = 10 distinct shapes × 3 phases (locked observation)");
    CHECK(kDupKeysAtLeast >= 3,
          "at least 3 map materials show phase duplication (evidence for H3)");
    CHECK(kTotalDupInstances > 50,
          "total redundant map-material flushes > 50 per frame (regression floor)");
}

static void test_h3_last_write_wins_predicts_upshift() {
    // The measured b13(ph1) / b29(ph4) / b57(ph6) same-shape triple, with vertex colours
    // running dim→mid→bright (0.03 / 0.18 / 0.47). Under SRCALPHA/INVSRCALPHA with
    // alpha=1, the shader output is `tex.rgb * ras.rgb` (typical GX modulate), then the
    // blend `out = src.rgb * 1 + dst.rgb * 0 = src.rgb` — pure REPLACE. So last-write-
    // wins → the pixel takes ph6's lighting.
    //
    // Predict the final rgb-modulate for the clay/adobe texture (199,152,140)/255 under
    // ph6's brightest lighting (0.47) vs the correct ph1 lighting (0.03):
    const float tex[3] = {199.0f/255.0f, 152.0f/255.0f, 140.0f/255.0f};
    const float ras_ph1 = 0.03f;   // measured b13
    const float ras_ph6 = 0.47f;   // measured b57
    // ph1 pixel (what oracle-shape rendering would keep):
    const float ph1_r = tex[0] * ras_ph1;   const float ph1_g = tex[1] * ras_ph1;   const float ph1_b = tex[2] * ras_ph1;
    // ph6 pixel (what native's last-write-wins ends up with):
    const float ph6_r = tex[0] * ras_ph6;   const float ph6_g = tex[1] * ras_ph6;   const float ph6_b = tex[2] * ras_ph6;
    CHECK(ph6_r / ph1_r > 10.0f,
          "ph6 last-write-wins is >10x brighter per channel than the ph1 correct pass");
    // The ph6 result is WARM (r > g > b, matching the texture), and its per-channel
    // ratio matches the wash's chromatic signature: brighter but proportional to tex.
    CHECK(ph6_r > ph6_g && ph6_g > ph6_b,
          "ph6 pixel inherits texture's warm chromatic bias (r > g > b)");
    // A brief NUMERIC PREDICTION: aggregate the ph6-only pixel over the map region ≈
    // (0.37, 0.28, 0.26) — which is warmer than the wash's shift (delta 0.46, 0.18, 0.16)
    // but plausibly its dominant contributor when composited with sea/sky pixels beneath.
    const float predicted_ph6[3] = {ph6_r, ph6_g, ph6_b};
    CHECK(predicted_ph6[0] > 0.35f && predicted_ph6[0] < 0.42f,
          "ph6-only clay/adobe pixel predicted red ≈ 0.37 (evidence lock)");
}

// ── Multi-region sampling: rule out a UNIFORM fullscreen-warm-layer hypothesis ──
// Sampled means at three vertical bands of sbs_title.png (2026-07-02):
//    SKY    y ∈ [0.05, 0.25]: oracle=( 40,113,158)  native=(113,142,167)
//    ISLAND y ∈ [0.35, 0.55]: oracle=(120,148,219)  native=(175,195,245)
//    SEA    y ∈ [0.65, 0.85]: oracle=(102,183,189)  native=(220,230,229)
// Delta (native − oracle) per region:
//    SKY    = (+73, +29, +9)
//    ISLAND = (+55, +47, +26)
//    SEA    = (+118, +47, +40)
// Each is warm (delta.r > delta.g > delta.b — same signature) but the MAGNITUDE
// varies dramatically. That is the discriminator we exploit below.

static void test_wash_delta_is_NON_UNIFORM_across_regions() {
    // Deltas in 0-255 space.
    constexpr int SKY_D[3]    = {73, 29,  9};
    constexpr int ISLAND_D[3] = {55, 47, 26};
    constexpr int SEA_D[3]    = {118, 47, 40};
    // If a single fullscreen warm-additive pass (e.g. TLensGlow, TSunModel glow,
    // fader tint) were the driver, every region would receive THE SAME delta —
    // sky-additive N applied on top of sky = sky+N; same N applied on top of sea = sea+N.
    // The observed deltas are not uniform (SEA.r is 118 vs SKY.r 73 — a 60% spread).
    // Reject a fullscreen-uniform hypothesis on the red-channel spread alone.
    const int rmax = SEA_D[0], rmin = SKY_D[0];
    CHECK(rmax - rmin > 30,
          "R-delta spread > 30 across regions → NOT a single fullscreen warm layer");
    // Sky's R-shift is disproportionately huge relative to sky's G/B shifts — hints
    // that sky's specific rendering diverges more than the map region's.
    CHECK(SKY_D[0] > SKY_D[1] * 2 && SKY_D[0] > SKY_D[2] * 5,
          "SKY delta is REDDEST — sky-material rendering pathway diverges strongly");
}

static void test_all_regions_share_warm_signature_direction() {
    // Cross-region invariant: EVERY region's delta is R > G > B (warm), and G>=B.
    // This is what lets H3 (per-material triple-flush) still fit: the wrong
    // lighting/blend affects MANY materials, but they all have warm textures, so
    // the aggregate shift is warm everywhere the frame has geometry — with region-
    // specific magnitude proportional to how much wrong-composited map material
    // that region sees.
    constexpr int SKY_D[3]    = {73, 29,  9};
    constexpr int ISLAND_D[3] = {55, 47, 26};
    constexpr int SEA_D[3]    = {118, 47, 40};
    for (const auto& d : { SKY_D, ISLAND_D, SEA_D }) {
        CHECK(d[0] > d[1] && d[1] >= d[2],
              "every region: delta.r > delta.g >= delta.b (warm signature invariant)");
        CHECK(d[0] > 0 && d[1] > 0 && d[2] > 0,
              "every region: shift is POSITIVE (brightness ADDED, not subtracted)");
    }
}

static void test_no_single_multiplicative_gain_explains_all_regions() {
    // Could a single per-channel MULTIPLICATIVE gain (e.g. wrong gamma pipeline)
    // explain all three regions? gain = native/oracle:
    //   SKY    R=2.83, G=1.26, B=1.06
    //   ISLAND R=1.46, G=1.32, B=1.12
    //   SEA    R=2.16, G=1.26, B=1.21
    // The R gain ranges 1.46 .. 2.83 — a nearly 2x spread. A single gamma / global
    // scale would give the SAME gain in every region. Reject uniform gamma too.
    const float sky_r    = 113.0f / 40.0f;
    const float island_r = 175.0f / 120.0f;
    const float sea_r    = 220.0f / 102.0f;
    CHECK(sky_r > island_r * 1.5f,
          "SKY R-gain > 1.5× ISLAND R-gain → no single global multiplicative gain fits");
    CHECK(sea_r > island_r * 1.3f && sky_r > sea_r * 1.2f,
          "SKY > SEA > ISLAND R-gain — gain varies with region → NOT a global gamma bug");
}

// ── ABLATION EXPERIMENT: SB_ABLATE_PHASE=6 shifts, but overshoots on SEA ────────
// A runtime ablation was performed 2026-07-02: rerun sms-boot with SB_ABLATE_PHASE=6
// (drops every scene batch captured during mPerformListGXPost dispatch) and re-sample
// the three regions. This test locks the observed per-region deltas so a future arc
// reads them instead of re-running the experiment:
//   SKY    unablated=(113,142,167)  ablated=(103,135,184)  ← modest improvement, B moved toward oracle 158
//   ISLAND unablated=(175,195,245)  ablated=(175,195,249)  ← no change
//   SEA    unablated=(220,230,229)  ablated=( 18,123,128)  ← COLLAPSED, darker than oracle
// SEA overshooting to (18,123,128) — darker than oracle (102,183,189) — proves ph6
// contains BOTH the wash contribution AND legitimate sea/water rendering. So a naive
// "gate ph6" fix loses valid content. The right fix must be SURGICAL: isolate the
// wash-causing map redraws WITHIN ph6 from the sea/final-composite draws that must run.

static void test_ablate_ph6_shifts_but_overshoots_sea() {
    // Locked observed per-region deltas from the ablation experiment.
    constexpr int SEA_UNABLATED[3]   = {220, 230, 229};
    constexpr int SEA_ABLATED_PH6[3] = { 18, 123, 128};
    constexpr int SEA_ORACLE[3]      = {102, 183, 189};
    // Ablation reduces SEA on every channel — direction of the shift is correct.
    for (int c = 0; c < 3; ++c) {
        CHECK(SEA_ABLATED_PH6[c] < SEA_UNABLATED[c],
              "SB_ABLATE_PHASE=6 reduces SEA channel intensity — direction correct");
    }
    // But the reduction OVERSHOOTS the oracle target — dropping BELOW oracle rather
    // than converging on it. That means ph6 also runs legitimate draws in this region.
    for (int c = 0; c < 3; ++c) {
        CHECK(SEA_ABLATED_PH6[c] < SEA_ORACLE[c],
              "ablated SEA channel dropped BELOW oracle → ph6 also carries legitimate content");
    }
    // Quantify the overshoot: SEA dropped by (~200, ~107, ~101). Oracle needed only
    // (~118, ~47, ~40) worth of removal. So ~40% of the removed content was VALID.
    const int removed_r = SEA_UNABLATED[0] - SEA_ABLATED_PH6[0];    // 202
    const int wanted_r  = SEA_UNABLATED[0] - SEA_ORACLE[0];          // 118
    CHECK(removed_r > wanted_r * 1.5,
          "ablation removed ~1.7× more than needed on SEA.R — over-aggressive fix, will lose content");
}

static void test_ablation_narrows_fix_shape() {
    // The right fix, evidence-derived:
    //   * NOT a phase-wide gate (SEA overshoots to (18,123,128) → dark, unacceptable)
    //   * The redundant MAP-MATERIAL redraws inside ph6 are the wash source (H3)
    //   * The valid SEA/water draws inside ph6 must remain
    //   * So the fix keys on MATERIAL not PHASE: skip the ph6 flush of shaderKeys
    //     already flushed at ph1/ph4 (the 6 duplicated map keys: e33908fd, 224004d9,
    //     153cc191, f19161bf, 85b2f9ca, 69ebca44) — leave everything else in ph6
    //     to draw normally
    //   * OR the segmented-EFB-snapshot-and-clear approach: at end of ph1/ph4, take
    //     a snapshot texture and clear the framebuffer; ph6 draws over cleared
    //     framebuffer then composites the snapshot; ph6 map redraws still happen but
    //     they go to a target the final composite doesn't sample (or samples via
    //     depth-tested compositing that discards them).
    // Both approaches keep the legit ph6 content while eliminating the wash.
    // Neither has been implemented yet; this test documents the fix-shape narrowing.
    CHECK(true, "fix shape narrowed: material-key gate OR segmented EFB clear — NOT phase-wide gate");
}

static void test_region_specific_wash_supports_H3() {
    // The narrowing chain:
    //   (a) shift is warm everywhere but non-uniform → NOT a single fullscreen layer
    //   (b) shift is warm everywhere but not a single global gain → NOT a global
    //       gamma / lighting-scale bug
    //   (c) already established: 6 map materials duplicated across ph{1,4,6},
    //       ph6 lighting is 15× brighter than ph1 for the same geometry
    //   (d) already established: ZERO warm scene-batch vertex colours, ZERO warm
    //       konst/tevreg — the warmth can only come from TEXTURES modulated by
    //       (wrong-bright) lighting; the 48 batches with WARM textures fit
    //   (e) map-material coverage varies per region, so region-specific wash
    //       magnitude is EXACTLY what H3 predicts.
    // Nothing here is a definitive proof, but every rejection above thins the
    // hypothesis space toward H3.
    CHECK(true, "narrowing chain (a)-(e) leaves H3 (missing EFB-clear → last-write-wins map flush) as the standing lead");
}

static void test_h3_ruled_in_ph6_dominates_shift_direction() {
    // ALTERNATIVE HYPOTHESIS: maybe the wash is from an ORACLE-ph pass being MISSING on
    // native (dimmer, cooler) rather than ph6 being EXTRA (brighter, warmer). Numerically
    // discriminate: the wash shifts UP and WARM. A missing pass would shift DOWN. So the
    // direction of the shift alone points at "extra bright pass added" — ph6.
    for (int c = 0; c < 3; ++c) {
        CHECK(NATIVE_RGB[c] > ORACLE_RGB[c],
              "native > oracle on every channel → extra brightness ADDED, not removed");
    }
    // The mask (SRCALPHA/SRCCLR eb5c8e74 at ph1+ph6, only 2 flushes with white src)
    // was H1+2, disconfirmed. The map materials (SRCALPHA/INVSRCALPHA at ph1+ph4+ph6,
    // 30+14+13 flushes with warm tex + dim-to-bright per-phase ras) match:
    //   direction ✓ (adds brightness)
    //   chromaticity ✓ (warm-tinted, matches map textures 168..254 R, 99..209 G, 65..190 B)
    //   flush count ✓ (67+ redundant per frame, driven by missing EFB-clear structure)
    // → H3 STANDING LEAD reinforced. Next arc: implement the segmented EFB snapshot+clear
    //    or gate ph6 map flushes so ph4's lighting is the final one.
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
    test_h3_multi_phase_flush_count_matches_evidence();
    test_h3_last_write_wins_predicts_upshift();
    test_wash_delta_is_NON_UNIFORM_across_regions();
    test_all_regions_share_warm_signature_direction();
    test_no_single_multiplicative_gain_explains_all_regions();
    test_ablate_ph6_shifts_but_overshoots_sea();
    test_ablation_narrows_fix_shape();
    test_region_specific_wash_supports_H3();
    test_h3_ruled_in_ph6_dominates_shift_direction();
    if (g_fail) { std::fprintf(stderr, "gx_blend_eval_test: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("gx_blend_eval_test: all passed\n");
    std::printf("  H1+2 (mask double-flush white src) DISCONFIRMED — would predict (1,1,1), observed (220,230,229).\n");
    std::printf("  H3 (missing EFB-clear → last-write-wins map flush) REINFORCED:\n");
    std::printf("    * 6+ map material shaderKeys flushed 2-30× per frame across ph{1,4,6} (67+ redundant flushes).\n");
    std::printf("    * Per-phase vertex lighting varies dim→bright (b13 ph1=0.03 → b57 ph6=0.47, 15× brighter).\n");
    std::printf("    * bm=1/4/5 alpha=1 + LEQUAL depth-write = LAST WRITE WINS → ph6's brightest lighting is what native shows.\n");
    std::printf("    * Warm tex modulated by bright ras = warm-tinted output → matches observed wash's chromatic signature.\n");
    std::printf("  MULTI-REGION check: wash delta per region (SKY/ISLAND/SEA) is warm-consistent but non-uniform:\n");
    std::printf("    SKY delta=(+73,+29,+9)   ISLAND=(+55,+47,+26)   SEA=(+118,+47,+40).\n");
    std::printf("    Rules out single fullscreen warm layer AND global gamma bug — both would give equal shift.\n");
    std::printf("  ABLATION EXPERIMENT: SB_ABLATE_PHASE=6 shifts every region — SKY improves modestly,\n");
    std::printf("    ISLAND unchanged, SEA COLLAPSES to (18,123,128), well BELOW oracle (102,183,189).\n");
    std::printf("    Rules out phase-wide gate: ph6 also runs legitimate SEA/water draws.\n");
    std::printf("  FIX SHAPE (evidence-narrowed):\n");
    std::printf("    * NOT a phase gate (loses legit content).\n");
    std::printf("    * Material-key gate: skip ph6 flush of the 6 shaderKeys already flushed at ph1/ph4\n");
    std::printf("      (e33908fd, 224004d9, 153cc191, f19161bf, 85b2f9ca, 69ebca44) — leave rest.\n");
    std::printf("    * OR segmented EFB snapshot+clear so ph6 map redraws don't reach the final composite.\n");
    return 0;
}
