#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/native_render/model.h>

#include <cstdint>

namespace sb::native_render {

enum class J3dMaskedToonMaterialResult : std::uint8_t {
    Success,
    UnsupportedColorBlock,
    UnsupportedColorChannels,
    UnsupportedSecondaryColors,
    UnsupportedTevBlock,
    UnsupportedStageCount,
    MissingTextureCoordinates,
    UnsupportedTextureBindings,
    UnsupportedColorProgram,
    MissingRegisterColor,
    MissingNormal,
    MissingLightingContext,
    UnsupportedRasterPolicy,
    MissingTextureBinding,
    InvalidTextureResource,
};

[[nodiscard]] const char*
j3d_masked_toon_material_result_name(J3dMaskedToonMaterialResult result) noexcept;
[[nodiscard]] J3dMaskedToonMaterialResult classify_j3d_masked_toon_material(
    const J3dMaterialState& state, const PictureTexture& primaryTexture,
    const PictureTexture& maskTexture, const PictureTexture& alternateTexture,
    const PictureTexture& lightRampTexture, const ModelLightingContext& lighting,
    LitMaskedToonMaterial& material) noexcept;

} // namespace sb::native_render
