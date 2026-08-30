#pragma once

#include <sunbright/native_render/model_context.h>

#include <cstdint>

namespace sb {

struct NativeJ3dSceneStats {
    std::uint64_t perspectiveDispatches = 0;
    std::uint64_t orthographicDispatches = 0;
    std::uint64_t unavailableDispatches = 0;
};

[[nodiscard]] const native_render::ModelSceneContext* current_native_j3d_scene() noexcept;
[[nodiscard]] NativeJ3dSceneStats native_j3d_scene_stats() noexcept;

} // namespace sb
