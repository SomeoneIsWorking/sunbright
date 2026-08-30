#include "native_j3d_scene.h"

#include <dolphin/os.h>

#include <algorithm>

namespace {

sb::native_render::ModelSceneContextStack g_scenes{};
sb::NativeJ3dSceneStats g_stats{};

} // namespace

namespace sb {

const native_render::ModelSceneContext* current_native_j3d_scene() noexcept {
    return g_scenes.current();
}

NativeJ3dSceneStats native_j3d_scene_stats() noexcept {
    return g_stats;
}

} // namespace sb

extern "C" void sb_native_j3d_scene_push(const float* projectionValues) {
    if (projectionValues == nullptr) {
        OSPanic(__FILE__, __LINE__,
                "semantic J3D draw dispatch received null TGraphics projection");
        return;
    }

    sb::native_render::Matrix4x4 projection{};
    std::copy_n(projectionValues, projection.value.size(), projection.value.begin());
    sb::native_render::ModelSceneContext context{};
    const sb::native_render::J3dProjectionResult result =
        sb::native_render::capture_j3d_scene_context(projection, context);
    bool pushed = false;
    if (result == sb::native_render::J3dProjectionResult::Perspective) {
        ++g_stats.perspectiveDispatches;
        pushed = g_scenes.push(context);
    } else if (result == sb::native_render::J3dProjectionResult::Orthographic) {
        ++g_stats.orthographicDispatches;
        pushed = g_scenes.push(context);
    } else {
        // Setup traversals can carry a draw cue before a camera initializes TGraphics. Suppress
        // any outer projection rather than inheriting stale camera state.
        ++g_stats.unavailableDispatches;
        pushed = g_scenes.push_empty();
    }
    if (!pushed) {
        OSPanic(__FILE__, __LINE__, "semantic J3D scene context stack overflow");
        return;
    }
}

extern "C" void sb_native_j3d_scene_pop(void) {
    if (!g_scenes.pop())
        OSPanic(__FILE__, __LINE__, "semantic J3D scene context stack underflow");
}
