#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/native_render/model.h>

#include <cstdint>

namespace sb::native_render {

enum class J3dLayeredMaterialResult : std::uint8_t {
    Success,
    UnsupportedColorBlock,
    UnsupportedColorChannels,
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
j3d_layered_material_result_name(J3dLayeredMaterialResult result) noexcept;
[[nodiscard]] J3dLayeredMaterialResult
classify_j3d_layered_material(const J3dMaterialState& state, const PictureTexture& baseTexture,
                              const PictureTexture& detailTexture,
                              const ModelLightingContext& lighting,
                              LitLayeredTexturedMaterial& material) noexcept;

} // namespace sb::native_render
