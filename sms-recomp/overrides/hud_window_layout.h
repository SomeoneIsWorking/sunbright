#pragma once

#include <cstdint>

namespace sunbright::hud {

struct HorizontalBounds {
    std::int32_t left;
    std::int32_t right;
};

struct CenteredWindowExtension {
    HorizontalBounds outer;
    HorizontalBounds content;
    std::int32_t matrix_shift_x;
};

CenteredWindowExtension extend_window_centered(HorizontalBounds outer, HorizontalBounds content,
                                               std::int32_t amount);

HorizontalBounds project_frame_to_scissor(HorizontalBounds outer, float matrix_scale_x,
                                          float matrix_translate_x, float projection_scale,
                                          float projection_center);

} // namespace sunbright::hud
