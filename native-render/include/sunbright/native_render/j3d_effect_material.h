#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/native_render/model.h>

#include <cstdint>

namespace sb::native_render {

enum class J3dEffectMaterialResult : std::uint8_t {
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
    UnsupportedRasterPolicy,
};

[[nodiscard]] const char* j3d_effect_material_result_name(J3dEffectMaterialResult result) noexcept;
[[nodiscard]] bool is_j3d_effect_material_program(const J3dTevStageState& stage) noexcept;
[[nodiscard]] J3dEffectMaterialResult
classify_j3d_effect_material(const J3dMaterialState& state, const PictureTexture& texture,
                             TexturedEffectMaterial& material) noexcept;

} // namespace sb::native_render
