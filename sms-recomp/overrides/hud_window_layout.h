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

} // namespace sunbright::hud
