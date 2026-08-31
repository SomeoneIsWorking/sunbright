#pragma once

#include <sunbright/native_render/model.h>

namespace sb::native_render {

// High-level values selected by Super Mario Sunshine's stage-light owner before it talks to GX.
// Both runtime adapters publish this same input, so the native renderer never consumes a console
// light object or compatibility-renderer register mirror.
struct J3dStageLightingInput {
    Matrix3x4 view{};
    Vec3 primaryWorldPosition{};
    Color primaryColor{1.0F, 1.0F, 1.0F, 1.0F};
    Color ambientColor{};
    bool effectEnabled = false;
    Vec3 effectWorldPosition{};
    Color effectColor{1.0F, 1.0F, 1.0F, 1.0F};
};

[[nodiscard]] ModelLightingContext
build_j3d_stage_lighting(const J3dStageLightingInput& input) noexcept;
void publish_j3d_stage_lighting(const J3dStageLightingInput& input) noexcept;
void clear_j3d_stage_lighting() noexcept;
[[nodiscard]] const ModelLightingContext* current_j3d_stage_lighting() noexcept;

} // namespace sb::native_render
