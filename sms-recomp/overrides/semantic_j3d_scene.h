#pragma once

#include <intrinsics.h>

#include <cstdint>

namespace sb::native_render {
struct ModelSceneContext;
}

namespace sb::recomp {

struct SemanticJ3dSceneStats {
    std::uint64_t perspectiveDispatches = 0;
    std::uint64_t orthographicDispatches = 0;
    std::uint64_t unavailableDispatches = 0;
};

[[nodiscard]] const native_render::ModelSceneContext* current_semantic_j3d_scene() noexcept;
[[nodiscard]] SemanticJ3dSceneStats semantic_j3d_scene_stats() noexcept;
void run_semantic_j3d_draw_dispatch(CPUState& cpu);

} // namespace sb::recomp
