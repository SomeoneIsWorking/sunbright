#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/native_render/model.h>

#include <cstdint>

namespace sb::native_render {

enum class J3dSpecularTexturedResult : std::uint8_t {
    Success,
    UnsupportedColorBlock,
    UnsupportedColorChannels,
    UnsupportedSecondaryColors,
    UnsupportedTevBlock,
    UnsupportedStageCount,
    MissingTextureCoordinate,
    UnsupportedTextureBinding,
    UnsupportedColorProgram,
    MissingNormal,
    MissingVertexColor,
    MissingLightingContext,
    UnsupportedRasterPolicy,
};

[[nodiscard]] const char*
j3d_specular_textured_result_name(J3dSpecularTexturedResult result) noexcept;
[[nodiscard]] J3dSpecularTexturedResult classify_j3d_specular_textured_material(
    const J3dMaterialState& state, const PictureTexture& texture,
    const ModelLightingContext& lighting, LitSpecularTexturedMaterial& material) noexcept;

} // namespace sb::native_render
