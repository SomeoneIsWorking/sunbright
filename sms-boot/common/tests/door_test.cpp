// door_test — spec-derived unit test for the TDoor::load port's "serialized-int to locked
// bool" predicate. Pure logic, Dolphin-free / no ROM.
//
// Spec (from scratch/decomp_door/801c24cc.c): the RE compares the 32-bit read against 0
// with `!= 0`. Any nonzero value → locked. Common wrong-operator regressions this test
// catches:
//   `> 0`: fails on 0xFFFFFFFF (signed -1) — should lock, would unlock.
//   `== 1`: fails on any value other than exactly 1.

#include "sms_boot_door.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

static void test_zero_unlocks() {
    CHECK(sb::door_locked_from_serialized(0) == false, "0 → unlocked");
}
static void test_one_locks() {
    CHECK(sb::door_locked_from_serialized(1) == true, "1 → locked");
}
static void test_max_locks() {
    // Signed -1 as unsigned 0xFFFFFFFF — the common "signed vs unsigned" regression.
    CHECK(sb::door_locked_from_serialized(0xFFFFFFFFu) == true, "0xFFFFFFFF → locked");
}
static void test_arbitrary_locks() {
    CHECK(sb::door_locked_from_serialized(42) == true, "42 → locked");
    CHECK(sb::door_locked_from_serialized(0x80000000u) == true, "high-bit only → locked");
}

int main() {
    test_zero_unlocks();
    test_one_locks();
    test_max_locks();
    test_arbitrary_locks();
    if (g_fail) { std::fprintf(stderr, "door_test: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("door_test: all passed\n");
    return 0;
}
