#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/native_render/model.h>

#include <cstdint>

namespace sb::native_render {

enum class J3dLitAlphaTintResult : std::uint8_t {
    Success,
    UnsupportedColorBlock,
    UnsupportedColorChannels,
    UnsupportedTevBlock,
    UnsupportedStageCount,
    MissingTextureCoordinate,
    UnsupportedTextureBinding,
    UnsupportedColorProgram,
    MissingTevColor,
    MissingNormal,
    MissingLightingContext,
    UnsupportedRasterPolicy,
};

[[nodiscard]] const char* j3d_lit_alpha_tint_result_name(J3dLitAlphaTintResult result) noexcept;
[[nodiscard]] J3dLitAlphaTintResult
classify_j3d_lit_alpha_tint_material(const J3dMaterialState& state, const PictureTexture& texture,
                                     const ModelLightingContext& lighting,
                                     LitAlphaTintMaterial& material) noexcept;

} // namespace sb::native_render
