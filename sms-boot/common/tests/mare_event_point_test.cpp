// mare_event_point_test — spec-derived constant regression test for the TMareEventPoint
// port. No math to check — the value here IS that the four constants in
// sms_boot_mare_event_point.h haven't drifted (typo, digit-swap, unit confusion, wrong slot).

#include "sms_boot_mare_event_point.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

// Verify the exact 32-bit word matches what the DOL's `lis 0x4000; ori 0x236` sequence
// materializes. A common regression here is swapping the low half (e.g. 0x40000326 vs
// 0x40000236) — this test catches it.
static void test_actor_type_bit_exact() {
    CHECK(sb::kMareEventPointActorType == 0x40000236u,
          "actor type must be exactly 0x40000236");
    // The high halfword is 0x4000 (the "trigger volume" tag family in SMS's actor-type
    // packing; misclassifying as 0x8000 would hide the point from enumeration entirely).
    CHECK((sb::kMareEventPointActorType >> 16) == 0x4000,
          "actor type high halfword must be 0x4000");
    CHECK((sb::kMareEventPointActorType & 0xFFFF) == 0x0236,
          "actor type low halfword must be 0x0236");
}

// Trigger cylinder shape: attack-cylinder empty (0, 0), damage-cylinder 300 × 600.
// A frequent bug class is transposing radius/height across attack↔damage; guard each slot.
static void test_no_attack_cylinder() {
    CHECK(sb::kMareEventPointAttackRadius == 0.0f, "attack radius must be 0");
    CHECK(sb::kMareEventPointAttackHeight == 0.0f, "attack height must be 0");
}
static void test_damage_cylinder_shape() {
    CHECK(sb::kMareEventPointDamageRadius == 300.0f, "damage radius must be exactly 300");
    CHECK(sb::kMareEventPointDamageHeight == 600.0f, "damage height must be exactly 600");
    // Ratio guard: height:radius should be 2:1 for these trigger volumes (designer intent
    // captured in the SDA constants). If a future edit changes ONE constant but not the
    // other, this ratio check flags the inconsistency.
    CHECK(sb::kMareEventPointDamageHeight
              == 2.0f * sb::kMareEventPointDamageRadius,
          "damage-cylinder height:radius must stay 2:1 (designer intent)");
}

int main() {
    test_actor_type_bit_exact();
    test_no_attack_cylinder();
    test_damage_cylinder_shape();
    if (g_fail) { std::fprintf(stderr, "mare_event_point_test: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("mare_event_point_test: all passed\n");
    return 0;
}
