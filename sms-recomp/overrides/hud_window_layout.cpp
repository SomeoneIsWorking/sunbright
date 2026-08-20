#include "hud_window_layout.h"

#include <limits>
#include <stdexcept>

namespace sunbright::hud {

namespace {

std::int32_t checked_add(std::int32_t value, std::int64_t delta) {
    const auto result = static_cast<std::int64_t>(value) + delta;
    if (result < std::numeric_limits<std::int32_t>::min() ||
        result > std::numeric_limits<std::int32_t>::max()) {
        throw std::overflow_error("J2D window extension exceeds signed 32-bit coordinates");
    }
    return static_cast<std::int32_t>(result);
}

} // namespace

CenteredWindowExtension extend_window_centered(HorizontalBounds outer, HorizontalBounds content,
                                               std::int32_t amount) {
    if (amount == std::numeric_limits<std::int32_t>::min()) {
        throw std::overflow_error("J2D window extension cannot negate INT32_MIN");
    }

    // draw_private does not place the frame at outer.left. It emits it at local x=0 with a width
    // of outer.right - outer.left, under the pane's global matrix. Move that one shared coordinate
    // system left and add the full 2*amount to both widths. The frame and fill then keep their
    // authored centres instead of growing from different origins.
    const std::int64_t extra_width = 2 * static_cast<std::int64_t>(amount);
    outer.right = checked_add(outer.right, extra_width);
    content.right = checked_add(content.right, extra_width);
    return {outer, content, -amount};
}

} // namespace sunbright::hud
