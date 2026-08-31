#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/native_render/model.h>

#include <cstdint>

namespace sb::native_render {

enum class J3dTintedLayeredMaterialResult : std::uint8_t {
    Success,
    UnsupportedColorBlock,
    UnsupportedColorChannels,
    UnsupportedSecondaryColors,
    UnsupportedTevBlock,
    UnsupportedStageCount,
    MissingTextureCoordinates,
    UnsupportedTextureBindings,
    UnsupportedColorProgram,
    MissingNormal,
    MissingLightingContext,
    UnsupportedRasterPolicy,
};

[[nodiscard]] const char*
j3d_tinted_layered_material_result_name(J3dTintedLayeredMaterialResult result) noexcept;
[[nodiscard]] J3dTintedLayeredMaterialResult classify_j3d_tinted_layered_material(
    const J3dMaterialState& state, const PictureTexture& baseTexture,
    const PictureTexture& detailTexture, const ModelLightingContext& lighting,
    LitTintedLayeredSpecularMaterial& material) noexcept;

} // namespace sb::native_render
