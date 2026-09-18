#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/native_render/model.h>

namespace sb::native_render {

enum class J3dInterpolatedRegisterResult : std::uint8_t {
    Success,
    UnsupportedColorBlock,
    UnsupportedTevBlock,
    UnsupportedStageCount,
    MissingTextureCoordinate,
    UnsupportedTextureBinding,
    UnsupportedColorProgram,
    MissingRegisterColor,
    UnsupportedRasterPolicy,
};

[[nodiscard]] const char*
j3d_interpolated_register_result_name(J3dInterpolatedRegisterResult result) noexcept;

// One image choosing per channel between two authored colour registers, offset by a colour
// constant's alpha, then averaged with a second image weighted by an authored fraction. No stage
// reads a rasterised channel, so the material's lit channel computes something nothing consumes
// and this rule does not gate on it.
[[nodiscard]] J3dInterpolatedRegisterResult classify_j3d_interpolated_register_material(
    const J3dMaterialState& state, const PictureTexture& blend, const PictureTexture& detail,
    InterpolatedRegisterMaterial& material) noexcept;

} // namespace sb::native_render
