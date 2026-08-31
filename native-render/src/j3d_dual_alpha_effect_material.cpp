#include <sunbright/native_render/j3d_dual_alpha_effect_material.h>

#include <sunbright/native_render/j3d_unlit_material.h>

#include <array>

namespace sb::native_render {
namespace {

constexpr std::uint16_t kPrimaryLightDiffuseChannel = 0x0706;
constexpr std::uint16_t kMaterialAlphaChannel = 0x0700;
constexpr std::uint8_t kColor0Alpha0 = 4;
constexpr std::array<std::uint8_t, 8> kStage0{0xC0, 0x08, 0xFF, 0xF2, 0xC1, 0x08, 0xFF, 0xC0};
constexpr std::array<std::uint8_t, 8> kStage1{0xC2, 0x18, 0xF4, 0x0A, 0xC3, 0x10, 0xF0, 0x50};
constexpr std::array<std::uint8_t, 8> kStage2{0xC4, 0x00, 0xFF, 0xF0, 0xC5, 0x00, 0xF4, 0x70};

Color color_from_s10_rgb(const std::array<std::int16_t, 4>& color) noexcept {
    constexpr float kScale = 1.0F / 255.0F;
    return {color[0] * kScale, color[1] * kScale, color[2] * kScale, 1.0F};
}

} // namespace

const char*
j3d_dual_alpha_effect_material_result_name(J3dDualAlphaEffectMaterialResult result) noexcept {
    switch (result) {
    case J3dDualAlphaEffectMaterialResult::Success:
        return "success";
    case J3dDualAlphaEffectMaterialResult::UnsupportedColorBlock:
        return "unsupported colour block";
    case J3dDualAlphaEffectMaterialResult::UnsupportedColorChannels:
        return "unsupported effect colour channels";
    case J3dDualAlphaEffectMaterialResult::UnsupportedTevBlock:
        return "unsupported colour-stage block";
    case J3dDualAlphaEffectMaterialResult::UnsupportedStageCount:
        return "unsupported colour-stage count";
    case J3dDualAlphaEffectMaterialResult::MissingTextureCoordinate:
        return "missing texture coordinate";
    case J3dDualAlphaEffectMaterialResult::UnsupportedTextureBinding:
        return "unsupported effect texture binding";
    case J3dDualAlphaEffectMaterialResult::UnsupportedColorProgram:
        return "unsupported effect colour program";
    case J3dDualAlphaEffectMaterialResult::MissingTevColor:
        return "missing effect TEV colour";
    case J3dDualAlphaEffectMaterialResult::MissingNormal:
        return "missing effect normal";
    case J3dDualAlphaEffectMaterialResult::MissingLightingContext:
        return "missing effect lighting context";
    case J3dDualAlphaEffectMaterialResult::UnsupportedRasterPolicy:
        return "unsupported raster policy";
    }
    return "unknown";
}

bool is_j3d_dual_alpha_effect_material(const J3dMaterialState& state) noexcept {
    return state.tevStageCount == 3 && state.tevStages[0].program == kStage0 &&
           state.tevStages[1].program == kStage1 && state.tevStages[2].program == kStage2;
}

J3dDualAlphaEffectMaterialResult classify_j3d_dual_alpha_effect_material(
    const J3dMaterialState& state, const PictureTexture& firstTexture,
    const PictureTexture& secondTexture, const ModelLightingContext& lighting,
    LitDualAlphaEffectMaterial& material) noexcept {
    if (!state.supportedColorBlock)
        return J3dDualAlphaEffectMaterialResult::UnsupportedColorBlock;
    if (!state.lightingEnabled || state.colorChannelCount != 1 ||
        state.colorChannelControl != kPrimaryLightDiffuseChannel ||
        state.alphaChannelControl != kMaterialAlphaChannel)
        return J3dDualAlphaEffectMaterialResult::UnsupportedColorChannels;
    if (!state.supportedTevBlock)
        return J3dDualAlphaEffectMaterialResult::UnsupportedTevBlock;
    if (!is_j3d_dual_alpha_effect_material(state))
        return J3dDualAlphaEffectMaterialResult::UnsupportedStageCount;
    if (state.textureCoordinateCount < 2 || state.tevStages[0].textureCoordinate != 0 ||
        state.tevStages[1].textureCoordinate != 1 || state.tevStages[0].textureMap != 0 ||
        state.tevStages[1].textureMap != 1 || state.tevStages[0].colorChannel != kColor0Alpha0 ||
        state.tevStages[1].colorChannel != kColor0Alpha0 ||
        state.tevStages[2].textureCoordinate != 0xFFU || state.tevStages[2].textureMap != 0xFFU ||
        state.tevStages[2].colorChannel != kColor0Alpha0 || firstTexture.resource == 0 ||
        firstTexture.width == 0 || firstTexture.height == 0 || secondTexture.resource == 0 ||
        secondTexture.width == 0 || secondTexture.height == 0)
        return J3dDualAlphaEffectMaterialResult::UnsupportedTextureBinding;
    if (!state.hasTevColors)
        return J3dDualAlphaEffectMaterialResult::MissingTevColor;
    if (!state.hasNormal)
        return J3dDualAlphaEffectMaterialResult::MissingNormal;
    if (lighting.pointLightCount == 0 || lighting.pointLightCount > lighting.pointLights.size())
        return J3dDualAlphaEffectMaterialResult::MissingLightingContext;
    ModelRasterPolicy raster{};
    if (classify_j3d_raster_policy(state, raster) != J3dRasterPolicyResult::Success)
        return J3dDualAlphaEffectMaterialResult::UnsupportedRasterPolicy;

    const Color first = color_from_s10_rgb(state.tevColorsS10[0]);
    const Color second = color_from_s10_rgb(state.tevColorsS10[1]);
    material.firstTexture = firstTexture;
    material.secondTexture = secondTexture;
    material.baseColor = color_from_rgba8(state.materialColorRgba8);
    material.ambientColor = state.usesMaterialAmbient ? color_from_rgba8(state.ambientColorRgba8)
                                                      : lighting.ambientColor;
    material.additiveColor = {2.0F * first.r * second.r, 2.0F * first.g * second.g,
                              2.0F * first.b * second.b, 0.0F};
    material.lighting = lighting;
    material.lighting.pointLightCount = 1;
    material.raster = raster;
    return J3dDualAlphaEffectMaterialResult::Success;
}

} // namespace sb::native_render
