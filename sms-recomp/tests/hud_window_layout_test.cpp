#include "overrides/hud_window_layout.h"

#include <cstdio>

namespace {

bool expect_equal(const char* label, std::int32_t actual, std::int32_t expected) {
    if (actual == expected)
        return true;
    std::fprintf(stderr, "FAIL: %s: got %d, expected %d\n", label, actual, expected);
    return false;
}

} // namespace

int main() {
    // J2DWindow::draw_private emits the frame from local x=0 using only outer.width(). The
    // extension therefore has to move the pane matrix left while adding the entire extra width
    // to each rect's right edge. Moving outer.left does not move the emitted frame.
    const auto result = sunbright::hud::extend_window_centered({.left = 40, .right = 507},
                                                               {.left = 8, .right = 435}, 107);

    bool ok = true;
    ok &= expect_equal("matrix shift", result.matrix_shift_x, -107);
    ok &= expect_equal("outer left", result.outer.left, 40);
    ok &= expect_equal("outer right", result.outer.right, 721);
    ok &= expect_equal("content left", result.content.left, 8);
    ok &= expect_equal("content right", result.content.right, 649);

    // The effective centres must stay put after applying the matrix shift. This is the invariant
    // that prevents the frame and content from becoming separately aligned side rectangles.
    ok &= expect_equal("effective frame centre x2",
                       2 * result.matrix_shift_x + (result.outer.right - result.outer.left),
                       507 - 40);
    ok &= expect_equal("effective content centre x2",
                       2 * result.matrix_shift_x + result.content.left + result.content.right,
                       8 + 435);

    // TGCConsole2 draws tet1/tet2 under a raw EFB scissor, while te_w goes through the 0.75-wide
    // 2D projection. Project the frame's actual matrix span into EFB coordinates; reusing the
    // unsqueezed pillar here would make the text run beyond the box.
    const auto scissor = sunbright::hud::project_frame_to_scissor(
        result.outer, 1.0f, static_cast<float>(result.matrix_shift_x), 0.75f, 320.0f);
    ok &= expect_equal("scissor left", scissor.left, 0);
    ok &= expect_equal("scissor right", scissor.right, 511);
    const auto control = sunbright::hud::project_frame_to_scissor({.left = 40, .right = 507}, 1.0f,
                                                                  40.0f, 1.0f, 320.0f);
    ok &= expect_equal("4:3 scissor left", control.left, 40);
    ok &= expect_equal("4:3 scissor right", control.right, 507);
    return ok ? 0 : 1;
}
