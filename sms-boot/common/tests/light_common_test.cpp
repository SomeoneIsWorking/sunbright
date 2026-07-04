// light_common_test — spec-derived unit test for the TLightCommon accessor ports
// (getLightPosition @0x80229ca0, getAmbColor @0x80229cec, getLightColor
// @0x80229d78). Pure logic, Dolphin-free / no ROM.
//
// Regressions this catches:
//   * "idx >= N" -> "idx > N" (fencepost) or "idx > 0" (nonsense clamp).
//   * unk28 / unk41 flag polarity flipped (would always use group, never local
//     override — every actor with a per-instance color/pos would silently fall
//     back to Light-Group[0]).
//   * Alpha-scale rounded (round-to-nearest) instead of truncated — a subtle
//     off-by-one when unk1C's product isn't an integer.
//   * Alpha-scale using round-half-to-even (banker's rounding) vs fctiwz truncate.

#include "sms_boot_lightcommon.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

int main() {
    using namespace sb::light_common;

    // clamp_local_idx: within-range passes through; out-of-range pins to 0.
    CHECK(clamp_local_idx(0, 4) == 0, "getLightPosition idx=0 pass-through");
    CHECK(clamp_local_idx(3, 4) == 3, "getLightPosition idx=3 (last in-range)");
    CHECK(clamp_local_idx(4, 4) == 0, "getLightPosition idx=4 (fencepost) → 0");
    CHECK(clamp_local_idx(99, 4) == 0, "getLightPosition idx=99 → 0");
    CHECK(clamp_local_idx(1, 2) == 1, "getAmbColor idx=1 (last in-range)");
    CHECK(clamp_local_idx(2, 2) == 0, "getAmbColor idx=2 (fencepost) → 0");

    // use_local — RE reads `lbz r0, 0x28(r3)` then `cmplwi r0, 0` then `beq else`.
    // So NONZERO → local, ZERO → group.
    CHECK(!use_local(0), "flag=0 → group");
    CHECK(use_local(1),  "flag=1 → local");
    CHECK(use_local(0xFF), "flag=0xFF → local");
    // A common mistake is `flag & 1` — this rules that out.
    CHECK(use_local(0x02), "flag=0x02 (bit0 clear) → still local");

    // scale_alpha_u8 — truncate, not round.
    CHECK(scale_alpha_u8(0xFF, 1.0f)  == 0xFF, "1.0 identity");
    CHECK(scale_alpha_u8(0xFF, 0.5f)  == 127,  "0.5 * 255 = 127.5 → 127 (truncate)");
    CHECK(scale_alpha_u8(0xFF, 0.0f)  == 0,    "0.0 * anything = 0");
    CHECK(scale_alpha_u8(0x80, 0.5f)  == 64,   "0.5 * 128 = 64.0");
    CHECK(scale_alpha_u8(100,  0.99f) == 99,   "0.99f * 100 = 99.0000... → truncate to 99 (float rep of 0.99 is slightly OVER)");
    CHECK(scale_alpha_u8(100,  0.999f) == 99,  "0.999f * 100 = 99.9... → truncate to 99");
    // Key: round-to-nearest would give 128 for 0.5*255, truncate gives 127.
    CHECK(scale_alpha_u8(0xFF, 0.5f)  != 128,  "MUST truncate (not round) — 128 would indicate rounding");
    // u8 wrap on overflow (scale > 1 pushes alpha above 255).
    CHECK(scale_alpha_u8(0xFF, 2.0f)  == 0xFE, "2.0 * 255 = 510 → int 510 → u8 wrap = 0xFE");

    if (g_fail) { std::fprintf(stderr, "light_common_test: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("light_common_test: all passed\n");
    return 0;
}
