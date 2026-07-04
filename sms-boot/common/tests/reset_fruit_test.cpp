// reset_fruit_test — spec-derived unit test for the MoveBG/MapObjBall TResetFruit port.
// Pure logic (the "should the Pinna Park branch fire?" predicate), Dolphin-free / no ROM / no GPU.
// The predicate is the SAME one TResetFruit::perform calls (via sb::reset_fruit_should_enter_pinna_park_branch).
//
// Spec-derived from scratch/decomp_resetfruit/801e21d0.c. The RE nests the branch as:
//     if (stage == 7) {
//         if (state == 6 OR vel_sq > threshold) {
//             /* clear flag 0x200 */
//         } else {
//             /* Yoshi-touch state machine — the branch this predicate matches TRUE for */
//         }
//     }
//     /* fall through */ TMapObjGeneral::perform(...)
// So the predicate returns TRUE iff we reach the Yoshi-touch else — stage == 7 AND state != 6
// AND vel_sq <= threshold. Each test hits one distinct exit of that decision tree so a wrong
// short-circuit shows up loudly.

#include "sms_boot_reset_fruit.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

// ── Each exit of the decision tree ───────────────────────────────────────────
static void test_wrong_stage_never_fires() {
    // File-select is stage 15. Pinna Park is stage 7. Any non-7 stage must NOT enter the branch.
    CHECK(!sb::reset_fruit_should_enter_pinna_park_branch(15, 0, 0.0f, 0.01f),
          "stage 15 (file-select) never fires the Pinna Park branch");
    CHECK(!sb::reset_fruit_should_enter_pinna_park_branch(1, 0, 0.0f, 0.01f),
          "stage 1 (Delfino) never fires the Pinna Park branch");
    CHECK(!sb::reset_fruit_should_enter_pinna_park_branch(0, 0, 0.0f, 0.01f),
          "stage 0 (debug room) never fires the Pinna Park branch");
}

static void test_state_6_short_circuits() {
    // state == 6 = fruit taken/held — RE flow enters the "clear flag 0x200" branch, NOT the
    // Yoshi-touch one. Predicate must return false even at stage 7 with zero velocity.
    CHECK(!sb::reset_fruit_should_enter_pinna_park_branch(7, /*state=*/6, /*vel_sq=*/0.0f, 0.01f),
          "state 6 at Pinna Park with vel_sq=0 → NO enter (state-6 short-circuit)");
}

static void test_velocity_above_threshold_short_circuits() {
    // Fast-moving fruit at Pinna Park — RE branches into the clear-flag path, not Yoshi-touch.
    CHECK(!sb::reset_fruit_should_enter_pinna_park_branch(7, /*state=*/0,
                                                          /*vel_sq=*/1.0f, /*thresh=*/0.01f),
          "high vel_sq at Pinna Park → NO enter (fast-moving fruit)");
}

static void test_stage7_at_rest_normal_state_fires() {
    // The one and only path where the predicate returns true.
    CHECK(sb::reset_fruit_should_enter_pinna_park_branch(7, /*state=*/0,
                                                         /*vel_sq=*/0.0f, /*thresh=*/0.01f),
          "stage 7, state 0, vel_sq 0 → ENTER branch");
    CHECK(sb::reset_fruit_should_enter_pinna_park_branch(7, /*state=*/11 /*=0xB, RE Yoshi-active*/,
                                                         /*vel_sq=*/0.005f, /*thresh=*/0.01f),
          "stage 7, state 11 (Yoshi-active), vel_sq below threshold → ENTER");
}

// ── Threshold boundary (subtle: <= vs <) ────────────────────────────────────
static void test_threshold_boundary_uses_strict_greater_than() {
    // The RE's condition is `threshold < vel_sq` (bail out) — the negation for "enter branch" is
    // `vel_sq <= threshold`. Equality → ENTER. A "fix" that flipped to `<` (strict) would exclude
    // exactly the resting-fruit case at the threshold and this test would fire.
    CHECK(sb::reset_fruit_should_enter_pinna_park_branch(7, 0, /*vel_sq=*/0.01f, /*thresh=*/0.01f),
          "vel_sq == threshold → ENTER (RE uses < not <=)");
    CHECK(!sb::reset_fruit_should_enter_pinna_park_branch(7, 0, /*vel_sq=*/0.011f, /*thresh=*/0.01f),
          "vel_sq slightly above threshold → NOT ENTER");
}

int main() {
    test_wrong_stage_never_fires();
    test_state_6_short_circuits();
    test_velocity_above_threshold_short_circuits();
    test_stage7_at_rest_normal_state_fires();
    test_threshold_boundary_uses_strict_greater_than();
    if (g_fail) { std::fprintf(stderr, "reset_fruit_test: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("reset_fruit_test: all passed\n");
    return 0;
}
