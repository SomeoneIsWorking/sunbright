#include "hud_window_layout.h"

#include <algorithm>
#include <cmath>
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

HorizontalBounds project_frame_to_scissor(HorizontalBounds outer, float matrix_scale_x,
                                          float matrix_translate_x, float projection_scale,
                                          float projection_center) {
    const auto width = static_cast<std::int64_t>(outer.right) - outer.left;
    if (width < 0) {
        throw std::invalid_argument("J2D frame has negative width");
    }
    const double start = static_cast<double>(projection_scale) * matrix_translate_x +
                         static_cast<double>(projection_center) * (1.0 - projection_scale);
    const double end = static_cast<double>(projection_scale) *
                           (matrix_translate_x + matrix_scale_x * static_cast<double>(width)) +
                       static_cast<double>(projection_center) * (1.0 - projection_scale);
    constexpr double kMinRoundable =
        static_cast<double>(std::numeric_limits<std::int32_t>::min()) - 0.5;
    constexpr double kMaxRoundable =
        static_cast<double>(std::numeric_limits<std::int32_t>::max()) + 0.5;
    if (!std::isfinite(start) || !std::isfinite(end) || start <= kMinRoundable ||
        start >= kMaxRoundable || end <= kMinRoundable || end >= kMaxRoundable) {
        throw std::overflow_error("projected J2D frame exceeds signed 32-bit coordinates");
    }
    const auto rounded_start = std::lround(start);
    const auto rounded_end = std::lround(end);
    if (rounded_start < std::numeric_limits<std::int32_t>::min() ||
        rounded_start > std::numeric_limits<std::int32_t>::max() ||
        rounded_end < std::numeric_limits<std::int32_t>::min() ||
        rounded_end > std::numeric_limits<std::int32_t>::max()) {
        throw std::overflow_error("projected J2D frame exceeds signed 32-bit coordinates");
    }
    return {static_cast<std::int32_t>(std::min(rounded_start, rounded_end)),
            static_cast<std::int32_t>(std::max(rounded_start, rounded_end))};
}

} // namespace sunbright::hud
