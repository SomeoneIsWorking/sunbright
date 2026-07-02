// imm_batch_emit_test — retroactive close-test for commit ca8d5f3, which
// removed the sms_boot_present.cpp stopgap that hard-dropped every captured
// immediate-mode batch with colorUpdate=0 (alpha-plane mask writer) or a
// DSTALPHA blend factor (src or dst factor 6/7). That drop surfaced as
// 3 MISSING signature buckets in parity_sweep drawdiff between the
// state-pinned file-select oracle and native captures:
//
//   (1, 6, 7)  DSTALPHA / INVDSTALPHA  — 14 oracle draws, 0 native
//   (1, 1, 1)  ONE      / ONE          —  4 oracle draws, 0 native
//   (1, 0, 4)  ZERO     / SRCALPHA     —  1 oracle draw,  0 native
//
// The predicate `sb::render::should_emit_imm_batch(const SbImmBatch&)` in
// gx_imm_xform.h is the drop condition, extracted so the fix is directly
// unit-testable at the level the divergence surfaced (per the 2026-07-02
// parity-TDD doctrine — test the drawdiff signal level, not internal helpers).
// Pre-ca8d5f3 this test would FAIL: the four inputs below all match the
// dropped classes.
//
// Doctrine reminder: this test does NOT run Dolphin, does NOT render, does
// NOT invoke sms-boot's present() end-to-end. It asserts the pure predicate
// used by present() — the SMALLEST unit that captures the fix.
#include "gx_imm_xform.h"
#include <cstdio>
#include <cstdlib>

using sb::render::SbImmBatch;

namespace {
int g_fails = 0;

// The pre-ca8d5f3 stopgap condition, encoded here VERBATIM from the removed lines
// (sms_boot_present.cpp:396-398 at ca8d5f3^). We assert that:
//   (a) the CURRENT predicate emits every category (should_emit_imm_batch returns true), AND
//   (b) the OLD condition would have DROPPED it (a RED gate baked into the test).
// So the test cannot silently pass by matching pre-fix behavior — a regression that
// re-introduces the drop makes assertion (a) fail. This is the "test asserts the ORACLE
// value" doctrine: current code = emit, historical baked-in stopgap = drop, assert both.
bool pre_ca8d5f3_dropped(const SbImmBatch& ib) {
    const bool readsDstAlpha = (ib.blendType == 1) &&
        (ib.blendSrc == 6 || ib.blendSrc == 7 || ib.blendDst == 6 || ib.blendDst == 7);
    return !ib.colorUpdate || readsDstAlpha;
}

void expect_emit(const char* label, const SbImmBatch& ib, bool expected_emit, bool expected_old_drop) {
    const bool got = sb::render::should_emit_imm_batch(ib);
    if (got != expected_emit) {
        std::fprintf(stderr, "FAIL %-42s expected emit=%d got=%d\n", label, (int)expected_emit, (int)got);
        ++g_fails;
    } else {
        std::fprintf(stderr, "ok   %-42s emit=%d\n", label, (int)got);
    }
    // Historical baked-in check: this input must have been dropped by the pre-fix stopgap,
    // proving the test is exercising a category the fix actually changed. If this fails, the
    // test itself is no longer a close-test — the input is not in the divergent class.
    const bool old_drop = pre_ca8d5f3_dropped(ib);
    if (old_drop != expected_old_drop) {
        std::fprintf(stderr, "FAIL %-42s pre-ca8d5f3 stopgap: expected drop=%d got=%d\n",
                     label, (int)expected_old_drop, (int)old_drop);
        ++g_fails;
    }
}

SbImmBatch make_batch(int blendSrc, int blendDst, bool colorUpdate, bool alphaUpdate = true) {
    SbImmBatch ib{};
    ib.vstart = 0; ib.vcount = 6;
    ib.textured = false; ib.image = nullptr;
    ib.w = 0; ib.h = 0; ib.fmt = 0; ib.wrapS = 0; ib.wrapT = 0; ib.linear = 0;
    ib.tlut = nullptr; ib.tlutfmt = 0;
    ib.blendType = 1;                // GX_BM_BLEND
    ib.blendSrc = (signed char)blendSrc;
    ib.blendDst = (signed char)blendDst;
    ib.colorUpdate = colorUpdate;
    ib.alphaUpdate = alphaUpdate;
    ib.dstAlphaForce = false;
    ib.dstAlphaVal = 0;
    ib.tev = nullptr;
    return ib;
}
} // namespace

int main() {
    // Category 1 — DSTALPHA/INVDSTALPHA composite (oracle bucket (1,6,7)).
    // Pre-fix: dropped because blendSrc==6. Post-fix: emitted.
    auto b_dstalpha_read = make_batch(6, 7, /*colorUpdate=*/true);
    // fmt: label, batch, expected_emit_now (should be true post-fix), expected_old_drop (was dropped pre-fix)
    expect_emit("dst-alpha reader (blend 6/7, color ON)", b_dstalpha_read, true, true);

    // Category 2 — DSTALPHA in dst factor slot (oracle bucket (1,4,6/7)).
    // Pre-fix: dropped because blendDst==7. Post-fix: emitted.
    auto b_invdstalpha_dst = make_batch(4, 7, /*colorUpdate=*/true);
    // fmt: label, batch, expected_emit_now (should be true post-fix), expected_old_drop (was dropped pre-fix)
    expect_emit("blend dst=INVDSTALPHA (color ON)", b_invdstalpha_dst, true, true);

    // Category 3 — colour-OFF alpha-plane mask writer (oracle bucket (1,1,0) cU=0).
    // Pre-fix: dropped because !colorUpdate. Post-fix: emitted; SDL3 GPU's
    // color_write_mask blocks RGB while permitting alpha.
    auto b_mask_writer = make_batch(1, 0, /*colorUpdate=*/false, /*alphaUpdate=*/true);
    // fmt: label, batch, expected_emit_now (should be true post-fix), expected_old_drop (was dropped pre-fix)
    expect_emit("mask writer (color OFF, alpha ON)", b_mask_writer, true, true);

    // Category 4 — ordinary alpha-blend UI draw (SRCALPHA/INVSRCALPHA). Both
    // pre-fix and post-fix should emit — regression guard on the common case.
    auto b_uibase = make_batch(4, 5, /*colorUpdate=*/true);
    // fmt: label, batch, expected_emit_now (should be true post-fix), expected_old_drop (was dropped pre-fix)
    expect_emit("SRCALPHA/INVSRCALPHA (color ON)", b_uibase, true, false);

    // Category 5 — DSTALPHA/ONE additive (oracle bucket (1,6,1)).
    // Pre-fix: dropped. Post-fix: emitted.
    auto b_dstalpha_add = make_batch(6, 1, /*colorUpdate=*/true);
    // fmt: label, batch, expected_emit_now (should be true post-fix), expected_old_drop (was dropped pre-fix)
    expect_emit("DSTALPHA/ONE additive (color ON)", b_dstalpha_add, true, true);

    if (g_fails) {
        std::fprintf(stderr, "\n%d FAIL(s) — the imm-batch drop stopgap has reappeared\n", g_fails);
        return 1;
    }
    std::fprintf(stderr, "\nall imm-batch emit categories pass\n");
    return 0;
}
