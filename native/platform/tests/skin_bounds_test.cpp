// skin_bounds_test — regression test for the file-select mangled-Mario fix (2026-07-01,
// commit 32a03fa). Pure logic, no Dolphin/GX/J3D state — see native/render/sms_boot_skin_bounds.h.
//
// The bug: sb_boot_capture_j3d's per-vertex skin-matrix bounds check used j3dSys.getModel()'s
// draw-matrix-table size instead of the CAPTURED SHAPE's own table (shape->mDrawMtxData). At the
// file-select picker, j3dSys.getModel() pointed at Mario's cap (table size 1) while the body shape
// being captured had its own table of 106 entries — every skin index >=1 got silently clamped to
// slot 0, collapsing every limb onto one joint's matrix.

#include "sms_boot_skin_bounds.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)

// ── 1. THE regression: shape's own (large) table wins over a mismatched (small) fallback. ──
static void test_shape_own_table_wins_over_stale_fallback() {
    // Mario's body: own table has 106 entries; j3dSys.getModel() (stale, points at the cap) has 1.
    int bound = sb::skin_drawmtx_bound(/*shape_has_own_table=*/true, /*shape_own_entry_num=*/106,
                                        /*fallback_model_entry_num=*/1);
    CHECK(bound == 106, "shape's own 106-entry table is used, not the stale 1-entry fallback");

    // A mid-range skin index (e.g. joint 19, as seen live at the settled picker) must resolve to
    // itself, not collapse to 0 — this is the exact symptom that produced the mangled render.
    int idx = sb::resolve_skin_index(/*di=*/19, bound);
    CHECK(idx == 19, "skin index 19 resolves to itself under the correct (106) bound");

    // Under the OLD (buggy) bound of 1, the same index used to wrongly clamp to 0 — assert that
    // regression explicitly so a future change can't silently reintroduce it unnoticed.
    int old_buggy_idx = sb::resolve_skin_index(/*di=*/19, /*bound=*/1);
    CHECK(old_buggy_idx == 0, "sanity: index 19 WOULD clamp to 0 under the old wrong bound of 1 "
                              "(documents what the bug looked like, not desired behavior)");
}

// ── 2. Fallback path: a shape with no own table yet uses the fallback (matches the original,
//       non-buggy corner case sb_boot_capture_j3d already handled). ──
static void test_no_own_table_uses_fallback() {
    int bound = sb::skin_drawmtx_bound(/*shape_has_own_table=*/false, /*shape_own_entry_num=*/0,
                                        /*fallback_model_entry_num=*/5);
    CHECK(bound == 5, "no shape-own table -> falls back to the model's table size");
}

// ── 3. Sentinel index (0xffff, "no per-vertex override") always resolves to slot 0. ──
static void test_sentinel_index_resolves_to_zero() {
    CHECK(sb::resolve_skin_index(0xffffu, 106) == 0, "sentinel 0xffff -> slot 0 regardless of bound");
}

// ── 4. Out-of-bounds index (beyond the table) clamps to 0, doesn't read out of range. ──
static void test_out_of_bounds_index_clamps_to_zero() {
    CHECK(sb::resolve_skin_index(200, /*bound=*/106) == 0, "index 200 >= bound 106 -> clamp to 0");
    CHECK(sb::resolve_skin_index(105, /*bound=*/106) == 105, "index 105 < bound 106 -> passes through");
}

int main() {
    test_shape_own_table_wins_over_stale_fallback();
    test_no_own_table_uses_fallback();
    test_sentinel_index_resolves_to_zero();
    test_out_of_bounds_index_clamps_to_zero();
    if (g_fail) { std::fprintf(stderr, "skin_bounds_test: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("skin_bounds_test: all passed\n");
    return 0;
}
