#include "native_raster_state.h"

#include <cstdlib>

namespace {

void require(bool condition) {
    if (!condition)
        std::abort();
}

SbrDepthState state_with_cull(unsigned cull) {
    SbrDepthState state{};
    state.cull = static_cast<uint8_t>(cull);
    return state;
}

} // namespace

int main() {
    require(sbr_native_raster_submits_triangles(state_with_cull(0)));
    require(sbr_native_raster_submits_triangles(state_with_cull(1)));
    require(sbr_native_raster_submits_triangles(state_with_cull(2)));

    // Negative control for the named defect: back-face culling still emits front faces, while
    // GX_CULL_ALL must emit no fragments at all. Mapping both modes to SDL_GPU_CULLMODE_BACK makes
    // them observably indistinguishable and submits geometry the GameCube rejects.
    require(!sbr_native_raster_submits_triangles(state_with_cull(3)));
    return 0;
}
