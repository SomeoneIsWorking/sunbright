// sunglass_fade_test — spec-derived unit test for the GC2D/SunGlass port. Pure logic,
// Dolphin-free / no ROM / no GPU / no gpMarDirector / no gamepad.
//
// Expected values are HAND-DERIVED from scratch/decomp_sunglass/8017d264.c (perform's fade
// step) — the exponent-bit-twiddle in the decompile is a standard PPC u32→f64→f32 dance that
// collapses to:  alpha_new = end + cur * (start - end) / total.
// The pure sb::sunglass_fade_* helpers are the SAME functions the shipping SunGlass.cpp calls
// (see sms_boot_sunglass.h + the routing in TSunGlass::perform) — a failure here is a real
// port bug, not a fork.

#include "sms_boot_sunglass.h"
#include <cstdio>
#include <cstdint>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

// ── 1. fade_step_alpha at the endpoints. ─────────────────────────────────────
static void test_step_at_cur_zero_returns_end() {
    // decompile: at cur=0 the multiplication is 0, alpha = end + 0 = end. Verify.
    CHECK(sb::sunglass_fade_step_alpha(/*start=*/100, /*end=*/0, /*cur=*/0, /*total=*/60) == 0,
          "cur=0 → alpha = end (0)");
    CHECK(sb::sunglass_fade_step_alpha(/*start=*/0, /*end=*/80, /*cur=*/0, /*total=*/60) == 80,
          "cur=0 → alpha = end (80, the SunGlass CTOR alpha)");
}

static void test_step_at_cur_equals_total_returns_start() {
    // decompile: at cur=total the ratio is 1, alpha = end + (start-end) = start.
    CHECK(sb::sunglass_fade_step_alpha(/*start=*/100, /*end=*/0, /*cur=*/60, /*total=*/60) == 100,
          "cur=total → alpha = start (100) — fade-in complete");
    CHECK(sb::sunglass_fade_step_alpha(/*start=*/0, /*end=*/80, /*cur=*/60, /*total=*/60) == 0,
          "cur=total → alpha = start (0) — fade-out complete");
}

static void test_step_midway_interpolates_linearly() {
    // At cur=30/total=60 (halfway), alpha should be end + (start-end)/2.
    // start=100, end=0 → alpha ≈ 50.
    CHECK(sb::sunglass_fade_step_alpha(100, 0, 30, 60) == 50, "halfway of 0→100 = 50");
    // start=0, end=80 (SunGlass fade-out from 80): halfway → alpha 40.
    CHECK(sb::sunglass_fade_step_alpha(0, 80, 30, 60) == 40, "halfway of 80→0 = 40");
}

static void test_step_signed_delta_when_start_less_than_end() {
    // start<end (a fade-in FROM ambient TO opaque overlay). The signed subtract in
    // sunglass_fade_step_alpha lets the interpolation go the "wrong way" too. Regression:
    // if a future edit dropped the signed cast, the u8 wrap would produce a hugely wrong
    // (>=255-clamped) value; this test catches that.
    // start=20, end=200: at cur=total the alpha = start = 20 (NOT wrap-around).
    CHECK(sb::sunglass_fade_step_alpha(20, 200, 60, 60) == 20,
          "signed delta: cur=total with start<end → start (20), not wrap");
    // At cur=0, alpha = end = 200 (the higher of the two).
    CHECK(sb::sunglass_fade_step_alpha(20, 200, 0, 60) == 200,
          "signed delta: cur=0 with start<end → end (200)");
    // Halfway (cur=30/60): alpha ≈ 200 + (20-200)/2 = 110.
    CHECK(sb::sunglass_fade_step_alpha(20, 200, 30, 60) == 110,
          "signed delta: halfway of 200→20 = 110");
}

static void test_step_total_zero_returns_end_safely() {
    // Defensive: startFade never sets total=0 at the GC side (unk22 is initialised to 100 in
    // the ctor and never zeroed), but a /0 in an inlined helper would trap. Assert we return
    // end without crashing.
    CHECK(sb::sunglass_fade_step_alpha(100, 42, 0, 0) == 42, "total=0 → return end (no /0)");
}

// ── 2. fade_advance's active/inactive transition. ────────────────────────────
static void test_advance_increments_while_below_total() {
    sb::FadeAdvance a = sb::sunglass_fade_advance(/*cur=*/0, /*total=*/60);
    CHECK(a.new_cur == 1, "cur 0 → 1");
    CHECK(a.new_active, "still active mid-fade");
    a = sb::sunglass_fade_advance(29, 60);
    CHECK(a.new_cur == 30, "cur 29 → 30");
    CHECK(a.new_active, "still active at 29/60");
}

static void test_advance_deactivates_when_cur_reaches_total() {
    // At the FINAL step (cur == total), the RE branches into the else and sets unk26 = 0.
    // Sensitivity: a wrong `>` vs `>=` here would leak one extra active frame or truncate one.
    sb::FadeAdvance a = sb::sunglass_fade_advance(/*cur=*/60, /*total=*/60);
    CHECK(!a.new_active, "cur==total → deactivate");
    CHECK(a.new_cur == 60, "cur unchanged when deactivating (RE branch doesn't increment)");
}

static void test_advance_deactivates_when_cur_above_total() {
    // Defensive: cur>total → still deactivate (should never happen at runtime).
    sb::FadeAdvance a = sb::sunglass_fade_advance(/*cur=*/61, /*total=*/60);
    CHECK(!a.new_active, "cur>total → deactivate");
}

// ── 3. End-to-end spec trace (a 60-frame fade-out from alpha 80). ────────────
static void test_end_to_end_60_frame_fadeout() {
    // Reproduces the SunGlass CTOR-driven initial fade-out: startFade(mode!=2, active=true)
    // sets start=getShineAlpha()=0, end=100. But at file-select the RE-observed ctor uses
    // TColor(0,0,0,80) — a distinct "resting alpha = 80" state that's independent of the fade.
    // Here we simulate: startFade with start=0, end=80, total=60 → visible alpha runs 80→0.
    uint16_t cur = 0;
    bool active = true;
    uint8_t start = 0, end = 80, total = 60;

    // Frame 0: alpha = 80 (end); increment to cur=1.
    uint8_t a = sb::sunglass_fade_step_alpha(start, end, cur, total);
    CHECK(a == 80, "e2e frame 0: alpha 80");
    sb::FadeAdvance adv = sb::sunglass_fade_advance(cur, total); cur = adv.new_cur; active = adv.new_active;
    CHECK(cur == 1 && active, "e2e frame 0: advanced");

    // Frame 30: alpha ≈ 40 (halfway).
    for (int i = 1; i < 30; ++i) { adv = sb::sunglass_fade_advance(cur, total); cur = adv.new_cur; active = adv.new_active; }
    a = sb::sunglass_fade_step_alpha(start, end, cur, total);
    CHECK(a == 40, "e2e frame 30 (cur=30): alpha 40 halfway");
    CHECK(active, "e2e frame 30: still active");

    // Frame 60: cur has reached total this tick — alpha shows start=0, but fade stays active
    // for ONE more tick (the RE's cur<total branch increments THIS frame; the else/deactivate
    // branch fires on the NEXT frame when advance sees cur==total). This is the subtle-but-real
    // spec — a "fix" that deactivated a frame early would visibly shorten the fade by 1/60s.
    for (int i = 30; i < 60; ++i) { adv = sb::sunglass_fade_advance(cur, total); cur = adv.new_cur; active = adv.new_active; }
    a = sb::sunglass_fade_step_alpha(start, end, cur, total);
    CHECK(a == 0, "e2e frame 60: alpha 0 (start reached)");
    CHECK(active, "e2e frame 60: still ACTIVE (deactivation is 1 tick later)");

    // Frame 61: advance sees cur==total → deactivate.
    adv = sb::sunglass_fade_advance(cur, total); cur = adv.new_cur; active = adv.new_active;
    CHECK(!active, "e2e frame 61: DEACTIVATED (advance branch fires)");
}

int main() {
    test_step_at_cur_zero_returns_end();
    test_step_at_cur_equals_total_returns_start();
    test_step_midway_interpolates_linearly();
    test_step_signed_delta_when_start_less_than_end();
    test_step_total_zero_returns_end_safely();

    test_advance_increments_while_below_total();
    test_advance_deactivates_when_cur_reaches_total();
    test_advance_deactivates_when_cur_above_total();

    test_end_to_end_60_frame_fadeout();

    if (g_fail) { std::fprintf(stderr, "sunglass_fade_test: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("sunglass_fade_test: all passed\n");
    return 0;
}
