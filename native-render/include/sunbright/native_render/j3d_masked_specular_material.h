#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/native_render/model.h>

namespace sb::native_render {

enum class J3dMaskedSpecularResult : std::uint8_t {
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

[[nodiscard]] const char* j3d_masked_specular_result_name(J3dMaskedSpecularResult result) noexcept;

// A signed-diffuse detail layer chosen against the directional highlight by a mask image's RGB.
// The mask supplies the blend weight per channel; the detail image is added to the weighted lit
// colour and offset before clamping. Two authored materials share this program and differ only in
// whether the mask image's alpha gates opacity, which the result carries as a flag rather than as
// a second family.
[[nodiscard]] J3dMaskedSpecularResult classify_j3d_masked_specular_material(
    const J3dMaterialState& state, const PictureTexture& maskTexture,
    const PictureTexture& detailTexture, const ModelLightingContext& lighting,
    LitMaskedSpecularMaterial& material) noexcept;

} // namespace sb::native_render
