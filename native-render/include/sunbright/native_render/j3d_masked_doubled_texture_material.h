#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/native_render/model.h>

namespace sb::native_render {

enum class J3dMaskedDoubledTextureResult : std::uint8_t {
    Success,
    UnsupportedColorBlock,
    Lighting,
    UnsupportedColorChannels,
    UnsupportedTevBlock,
    UnsupportedStageCount,
    MissingTextureCoordinate,
    UnsupportedTextureBinding,
    UnsupportedColorProgram,
    UnsupportedRasterPolicy,
};

[[nodiscard]] const char*
j3d_masked_doubled_texture_result_name(J3dMaskedDoubledTextureResult result) noexcept;

// One texture doubled against the rasterised channel for colour, with an earlier texture
// contributing only opacity. The first stage computes a colour the second overwrites rather than
// combines with, so that colour is authored to be discarded -- which is exactly why this is not
// the doubled pair, whose second stage multiplies what the first produced.
[[nodiscard]] J3dMaskedDoubledTextureResult classify_j3d_masked_doubled_texture_material(
    const J3dMaterialState& state, const PictureTexture& opacity, const PictureTexture& color,
    MaskedDoubledTextureMaterial& material) noexcept;

} // namespace sb::native_render
