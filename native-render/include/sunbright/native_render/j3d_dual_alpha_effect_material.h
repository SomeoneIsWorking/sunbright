#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/native_render/model.h>

#include <cstdint>

namespace sb::native_render {

enum class J3dDualAlphaEffectMaterialResult : std::uint8_t {
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

[[nodiscard]] const char*
j3d_dual_alpha_effect_material_result_name(J3dDualAlphaEffectMaterialResult result) noexcept;
[[nodiscard]] bool is_j3d_dual_alpha_effect_material(const J3dMaterialState& state) noexcept;
[[nodiscard]] J3dDualAlphaEffectMaterialResult classify_j3d_dual_alpha_effect_material(
    const J3dMaterialState& state, const PictureTexture& firstTexture,
    const PictureTexture& secondTexture, const ModelLightingContext& lighting,
    LitDualAlphaEffectMaterial& material) noexcept;

} // namespace sb::native_render
