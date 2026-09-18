#include <sunbright/native_render/j3d_unlit_effect_material.h>

#include <sunbright/native_render/j3d_unlit_material.h>

#include <array>

namespace sb::native_render {
namespace {

constexpr std::uint16_t kMaterialColorChannel = 0x0700;
constexpr std::uint16_t kVertexColorChannel = 0x0701;
constexpr std::uint16_t kMaterialAlphaChannel = 0x0700;
constexpr std::uint8_t kColor0Alpha0 = 4;
// prev = clamp(tex.rgb * c0.rgb), alpha prev = clamp(tex.a * ras.a).
constexpr std::array<std::uint8_t, 8> kRegisterTimesTexture{0xC0, 0x08, 0xF8, 0x2F,
                                                            0xC1, 0x08, 0xF2, 0xF0};

} // namespace

const char* j3d_unlit_effect_result_name(J3dUnlitEffectResult result) noexcept {
    switch (result) {
    case J3dUnlitEffectResult::Success:
        return "success";
    case J3dUnlitEffectResult::UnsupportedColorBlock:
        return "unsupported colour block";
    case J3dUnlitEffectResult::Lighting:
        return "lighting";
    case J3dUnlitEffectResult::UnsupportedColorChannels:
        return "unsupported unlit effect colour channels";
    case J3dUnlitEffectResult::UnsupportedTevBlock:
        return "unsupported colour-stage block";
    case J3dUnlitEffectResult::UnsupportedStageCount:
        return "unsupported colour-stage count";
    case J3dUnlitEffectResult::MissingTextureCoordinate:
        return "missing texture coordinate";
    case J3dUnlitEffectResult::UnsupportedTextureBinding:
        return "unsupported texture binding";
    case J3dUnlitEffectResult::UnsupportedColorProgram:
        return "unsupported unlit effect colour program";
    case J3dUnlitEffectResult::MissingRegisterColor:
        return "missing colour register";
    case J3dUnlitEffectResult::UnsupportedRasterPolicy:
        return "unsupported raster policy";
    }
    return "unknown";
}

J3dUnlitEffectResult classify_j3d_unlit_effect_material(const J3dMaterialState& state,
                                                        const PictureTexture& texture,
                                                        TexturedEffectMaterial& material) noexcept {
    if (!state.supportedColorBlock) {
        return J3dUnlitEffectResult::UnsupportedColorBlock;
    }
    if (state.lightingEnabled) {
        return J3dUnlitEffectResult::Lighting;
    }
    // The stage takes its colour from a register and its alpha from the raster, so the alpha
    // channel decides what is drawn and the colour channel cannot: nothing reads its output. Both
    // authored spellings of that unread channel are admitted for exactly that reason.
    if (state.colorChannelCount != 1 ||
        (state.colorChannelControl != kMaterialColorChannel &&
         state.colorChannelControl != kVertexColorChannel) ||
        state.alphaChannelControl != kMaterialAlphaChannel) {
        return J3dUnlitEffectResult::UnsupportedColorChannels;
    }
    if (!state.supportedTevBlock) {
        return J3dUnlitEffectResult::UnsupportedTevBlock;
    }
    if (state.tevStageCount != 1) {
        return J3dUnlitEffectResult::UnsupportedStageCount;
    }
    const J3dTevStageState& stage = state.tevStages[0];
    if (state.textureCoordinateCount == 0) {
        return J3dUnlitEffectResult::MissingTextureCoordinate;
    }
    if (j3d_texture_number_for_map(state, stage.textureMap) == 0xFFFFU ||
        stage.textureCoordinate > 1 || stage.textureCoordinate >= state.textureCoordinateCount ||
        stage.colorChannel != kColor0Alpha0 || texture.resource == 0 || texture.width == 0 ||
        texture.height == 0) {
        return J3dUnlitEffectResult::UnsupportedTextureBinding;
    }
    if (stage.program != kRegisterTimesTexture) {
        return J3dUnlitEffectResult::UnsupportedColorProgram;
    }
    if (!state.hasTevColors) {
        return J3dUnlitEffectResult::MissingRegisterColor;
    }
    ModelRasterPolicy raster{};
    if (classify_j3d_raster_policy(state, raster) != J3dRasterPolicyResult::Success) {
        return J3dUnlitEffectResult::UnsupportedRasterPolicy;
    }

    const Color registerColor = color_from_s10(state.tevColorsS10[0]);
    material.texture = texture;
    material.textureCoordinates = stage.textureCoordinate == 1 ? ModelTextureCoordinates::Secondary
                                                               : ModelTextureCoordinates::Primary;
    material.alphaMode = ModelTextureAlphaMode::MultiplyTexture;
    // The stage multiplies the texture's own alpha by the raster's, which this material's alpha
    // channel takes from the material colour register.
    material.modulation = {registerColor.r, registerColor.g, registerColor.b,
                           color_from_rgba8(state.materialColorRgba8).a};
    material.additive = {};
    material.raster = raster;
    return J3dUnlitEffectResult::Success;
}

} // namespace sb::native_render
