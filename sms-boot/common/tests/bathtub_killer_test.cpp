// bathtub_killer_test — spec-derived unit test for the
// TBathtubKillerManager::countActiveKillers port (@0x8012f204).
//
// Regressions this catches:
//   * min replaced by max on the params clamp — over-scans dead entries past the
//     params-declared active window and inflates the count.
//   * NULL-params guard inverted — with unk38 == nullptr, the port would
//     read params->0xa4 (SEGV) or clamp to 0.
//   * counting DEAD instead of ALIVE — the RE increments in the `beq` branch
//     after the `& 0x1` test, i.e. when the low bit is CLEAR.
//   * off-by-one in the loop bound (< vs <=).

#include "sms_boot_bathtub_killer.h"
#include <cstdio>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

int main() {
    using namespace sb::bathtub_killer;

    CHECK(kLiveFlagDead == 1, "LIVE_FLAG_DEAD is bit 0");

    // clamp_limit — the min/params guard.
    CHECK(clamp_limit(10, false, 0)  == 10, "no-params → obj_num");
    CHECK(clamp_limit(10, false, 99) == 10, "no-params: params_active ignored");
    CHECK(clamp_limit(10, true,  5)  == 5,  "params<obj → params");
    CHECK(clamp_limit(10, true, 15)  == 10, "params>obj → obj (min semantics)");
    CHECK(clamp_limit(10, true, 10)  == 10, "params==obj → boundary passes");
    CHECK(clamp_limit(0,  true,  5)  == 0,  "empty manager → 0 no matter what");
    CHECK(clamp_limit(10, true,  0)  == 0,  "params=0 → 0 (fully quiescent)");

    // count_active — walks up to limit, counts entries where predicate is false.
    // All alive.
    {
        auto never = [](int) { return false; };
        CHECK(count_active(5, false, 0, never)  == 5, "5 alive, no params → 5");
        CHECK(count_active(5, true,  3, never)  == 3, "5 slots but params=3 → 3 alive");
    }
    // All dead.
    {
        auto always = [](int) { return true; };
        CHECK(count_active(5, false, 0, always) == 0, "all dead → 0");
        CHECK(count_active(5, true,  3, always) == 0, "all dead, params=3 → 0");
    }
    // Mixed: dead on even indices, alive on odd.
    {
        auto even_dead = [](int i) { return (i & 1) == 0; };
        // Indices 0..4: dead alive dead alive dead → 2 alive.
        CHECK(count_active(5, false, 0, even_dead) == 2, "even-dead pattern, 5 slots → 2 alive");
        // With params=3: indices 0..2 → dead alive dead → 1 alive.
        CHECK(count_active(5, true, 3, even_dead)  == 1, "even-dead pattern, params=3 → 1");
        // Params clamped ABOVE obj_num should NOT enable more iterations.
        CHECK(count_active(3, true, 99, even_dead) == 1, "params=99 clamped to obj_num=3");
    }
    // Zero-length iterations.
    {
        auto any = [](int) { return false; };
        CHECK(count_active(0, false, 0,  any) == 0, "obj_num=0 → 0");
        CHECK(count_active(0, true,  99, any) == 0, "obj_num=0 wins even with params>0");
    }

    if (g_fail) { std::fprintf(stderr, "bathtub_killer_test: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("bathtub_killer_test: all passed\n");
    return 0;
}
