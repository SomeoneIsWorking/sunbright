#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/native_render/model.h>

#include <cstdint>

namespace sb::native_render {

enum class J3dAlphaMaskedMaterialResult : std::uint8_t {
    Success,
    UnsupportedColorBlock,
    UnsupportedColorChannels,
    UnsupportedTevBlock,
    UnsupportedStageCount,
    MissingTextureCoordinate,
    UnsupportedTextureBinding,
    UnsupportedColorProgram,
    UnsupportedRegisterColor,
    MissingNormal,
    UnsupportedRasterPolicy,
};

[[nodiscard]] const char*
j3d_alpha_masked_material_result_name(J3dAlphaMaskedMaterialResult result) noexcept;
[[nodiscard]] J3dAlphaMaskedMaterialResult
classify_j3d_alpha_masked_material(const J3dMaterialState& state, const PictureTexture& texture,
                                   AlphaMaskedColorMaterial& material) noexcept;

} // namespace sb::native_render
