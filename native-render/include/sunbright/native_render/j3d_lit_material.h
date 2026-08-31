#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/native_render/model.h>

#include <cstdint>

namespace sb::native_render {

enum class J3dLitTexturedResult : std::uint8_t {
    Success,
    UnsupportedColorBlock,
    UnsupportedColorChannels,
    UnsupportedTevBlock,
    MultipleTevStages,
    MissingTextureCoordinate,
    UnsupportedTextureBinding,
    UnsupportedColorProgram,
    MissingNormal,
    MissingVertexColor,
    MissingLightingContext,
    UnsupportedRasterPolicy,
};

[[nodiscard]] const char* j3d_lit_textured_result_name(J3dLitTexturedResult result) noexcept;
[[nodiscard]] J3dLitTexturedResult
classify_j3d_lit_textured_material(const J3dMaterialState& state, const PictureTexture& texture,
                                   const ModelLightingContext& lighting,
                                   LitTexturedMaterial& material) noexcept;

} // namespace sb::native_render
