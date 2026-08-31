#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/native_render/model.h>

#include <cstdint>

namespace sb::native_render {

enum class J3dLitAlphaMaskResult : std::uint8_t {
    Success,
    UnsupportedColorBlock,
    UnsupportedColorChannels,
    UnsupportedTevBlock,
    UnsupportedStageCount,
    MissingTextureCoordinates,
    UnsupportedTextureBindings,
    UnsupportedColorProgram,
    UnsupportedRegisterColor,
    MissingNormal,
    MissingLightingContext,
    UnsupportedRasterPolicy,
};

[[nodiscard]] const char* j3d_lit_alpha_mask_result_name(J3dLitAlphaMaskResult result) noexcept;
[[nodiscard]] J3dLitAlphaMaskResult classify_j3d_lit_alpha_mask_material(
    const J3dMaterialState& state, const PictureTexture& colorTexture,
    const PictureTexture& alphaMaskTexture, const ModelLightingContext& lighting,
    LitTexturedAlphaMaskMaterial& material) noexcept;

} // namespace sb::native_render
