#include "semantic_j3d_scene.h"

#include "j3d_scene_projection_adapter.h"

#include "../runtime/sb_assert.h"

#include <sunbright/native_render/model_context.h>
#include <sunbright/native_render/semantic_sink.h>

extern "C" void func_802fcc94(CPUState&); // JDrama::TViewObj::testPerform

namespace sb::recomp {
namespace {

native_render::ModelSceneContextStack g_scenes{};
SemanticJ3dSceneStats g_stats{};

class SceneScope {
  public:
    explicit SceneScope(const CPUState& cpu) {
        constexpr u32 kDrawCue = 8;
        if ((cpu.gpr[4] & kDrawCue) == 0 || !native_render::has_semantic_sink())
            return;

        native_render::ModelSceneContext context{};
        const GuestJ3dSceneProjectionResult result =
            capture_guest_j3d_scene_projection(live_guest_byte_reader(), cpu.gpr[5], context);
        SB_ASSERT(result != GuestJ3dSceneProjectionResult::Unreadable,
                  "semantic J3D draw dispatch cannot read TGraphics projection: graphics=%08x",
                  cpu.gpr[5]);
        if (result == GuestJ3dSceneProjectionResult::Perspective) {
            ++g_stats.perspectiveDispatches;
            active_ = g_scenes.push(context);
        } else if (result == GuestJ3dSceneProjectionResult::Orthographic) {
            ++g_stats.orthographicDispatches;
            active_ = g_scenes.push(context);
        } else {
            // Draw-cue setup traversals can precede the first camera write. An explicit empty
            // entry prevents them from inheriting an unrelated outer scene projection.
            ++g_stats.unavailableDispatches;
            active_ = g_scenes.push_empty();
        }
        SB_ASSERT(active_, "semantic J3D scene context stack overflow");
    }

    ~SceneScope() {
        if (active_)
            SB_ASSERT(g_scenes.pop(), "semantic J3D scene context stack underflow");
    }

    SceneScope(const SceneScope&) = delete;
    SceneScope& operator=(const SceneScope&) = delete;

  private:
    bool active_ = false;
};

} // namespace

const native_render::ModelSceneContext* current_semantic_j3d_scene() noexcept {
    return g_scenes.current();
}

SemanticJ3dSceneStats semantic_j3d_scene_stats() noexcept {
    return g_stats;
}

void run_semantic_j3d_draw_dispatch(CPUState& cpu) {
    SceneScope scene(cpu);
    func_802fcc94(cpu);
}

} // namespace sb::recomp
