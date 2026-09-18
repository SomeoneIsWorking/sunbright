#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/native_render/model.h>

namespace sb::native_render {

enum class J3dTintedTextureSumResult : std::uint8_t {
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
j3d_tinted_texture_sum_result_name(J3dTintedTextureSumResult result) noexcept;

// Two texture layers, each scaled by its own authored colour constant and summed, with opacity
// taken from both images through one colour register. Neither stage reads a rasterised channel, so
// whatever the material's channels are lit to compute goes nowhere and this rule does not gate on
// them -- the program itself is the whole specification.
[[nodiscard]] J3dTintedTextureSumResult
classify_j3d_tinted_texture_sum_material(const J3dMaterialState& state, const PictureTexture& first,
                                         const PictureTexture& second,
                                         TintedTextureSumMaterial& material) noexcept;

} // namespace sb::native_render
