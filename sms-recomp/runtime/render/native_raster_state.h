#pragma once

#include "native_render.h"

// Whether a triangle draw reaches the SDL GPU backend. GX_CULL_ALL has no direct SDL rasterizer
// equivalent, so this policy must decide whether the draw is submitted at all.
[[nodiscard]] constexpr bool
sbr_native_raster_submits_triangles(const SbrDepthState& state) noexcept {
    return (state.cull & 3U) != 3U;
}
