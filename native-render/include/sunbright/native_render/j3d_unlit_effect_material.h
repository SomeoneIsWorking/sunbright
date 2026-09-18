#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/native_render/model.h>

namespace sb::native_render {

enum class J3dUnlitEffectResult : std::uint8_t {
    Success,
    UnsupportedColorBlock,
    Lighting,
    UnsupportedColorChannels,
    UnsupportedTevBlock,
    UnsupportedStageCount,
    MissingTextureCoordinate,
    UnsupportedTextureBinding,
    UnsupportedColorProgram,
    MissingRegisterColor,
    UnsupportedRasterPolicy,
};

[[nodiscard]] const char* j3d_unlit_effect_result_name(J3dUnlitEffectResult result) noexcept;

// One decoded texture modulated by an authored colour register, with no lighting. It publishes the
// same semantic material as the lit effect family -- a texture times an authored colour is the same
// thing however the channel that feeds it was authored -- but it is a separate rule because the
// authored state it accepts shares none of that family's gates.
[[nodiscard]] J3dUnlitEffectResult
classify_j3d_unlit_effect_material(const J3dMaterialState& state, const PictureTexture& texture,
                                   TexturedEffectMaterial& material) noexcept;

} // namespace sb::native_render
