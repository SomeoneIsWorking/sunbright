#include <sunbright/native_render/j3d_effect_material.h>

#include <sunbright/native_render/j3d_unlit_material.h>

#include <array>

namespace sb::native_render {
namespace {

constexpr std::uint16_t kPrimaryLightDiffuseChannel = 0x0706;
constexpr std::uint16_t kMaterialAlphaChannel = 0x0700;
constexpr std::uint8_t kColor0Alpha0 = 4;
constexpr std::uint8_t kKonstColor0 = 0x0C;
constexpr std::array<std::uint8_t, 8> kConstantTimesTexture{0xC0, 0x08, 0xFE, 0x8F,
                                                            0xC1, 0x08, 0xE6, 0x70};

Color color_from_s10(const std::array<std::int16_t, 4>& color) noexcept {
    constexpr float kScale = 1.0F / 255.0F;
    return {color[0] * kScale, color[1] * kScale, color[2] * kScale, color[3] * kScale};
}

} // namespace

const char* j3d_effect_material_result_name(J3dEffectMaterialResult result) noexcept {
    switch (result) {
    case J3dEffectMaterialResult::Success:
        return "success";
    case J3dEffectMaterialResult::UnsupportedColorBlock:
        return "unsupported colour block";
    case J3dEffectMaterialResult::UnsupportedColorChannels:
        return "unsupported effect colour channels";
    case J3dEffectMaterialResult::UnsupportedTevBlock:
        return "unsupported colour-stage block";
    case J3dEffectMaterialResult::UnsupportedStageCount:
        return "unsupported colour-stage count";
    case J3dEffectMaterialResult::MissingTextureCoordinate:
        return "missing texture coordinate";
    case J3dEffectMaterialResult::UnsupportedTextureBinding:
        return "unsupported effect texture binding";
    case J3dEffectMaterialResult::UnsupportedColorProgram:
        return "unsupported effect colour program";
    case J3dEffectMaterialResult::MissingTevColor:
        return "missing effect TEV colour";
    case J3dEffectMaterialResult::MissingNormal:
        return "missing effect normal";
    case J3dEffectMaterialResult::UnsupportedRasterPolicy:
        return "unsupported raster policy";
    }
    return "unknown";
}

J3dEffectMaterialResult classify_j3d_effect_material(const J3dMaterialState& state,
                                                     const PictureTexture& texture,
                                                     TexturedEffectMaterial& material) noexcept {
    if (!state.supportedColorBlock)
        return J3dEffectMaterialResult::UnsupportedColorBlock;
    if (!state.lightingEnabled || state.colorChannelCount != 1 ||
        state.colorChannelControl != kPrimaryLightDiffuseChannel ||
        state.alphaChannelControl != kMaterialAlphaChannel)
        return J3dEffectMaterialResult::UnsupportedColorChannels;
    if (!state.supportedTevBlock)
        return J3dEffectMaterialResult::UnsupportedTevBlock;
    if (state.tevStageCount != 1)
        return J3dEffectMaterialResult::UnsupportedStageCount;
    const J3dTevStageState& stage = state.tevStages[0];
    if (state.textureCoordinateCount == 0)
        return J3dEffectMaterialResult::MissingTextureCoordinate;
    if (state.textureBindings[0].textureNumber == 0xFFFFU || stage.textureCoordinate != 0 ||
        stage.textureMap != 0 || stage.colorChannel != kColor0Alpha0 || texture.resource == 0 ||
        texture.width == 0 || texture.height == 0)
        return J3dEffectMaterialResult::UnsupportedTextureBinding;
    if (stage.program != kConstantTimesTexture || stage.konstColorSelection != kKonstColor0)
        return J3dEffectMaterialResult::UnsupportedColorProgram;
    if (!state.hasTevColors)
        return J3dEffectMaterialResult::MissingTevColor;
    if (!state.hasNormal)
        return J3dEffectMaterialResult::MissingNormal;
    ModelRasterPolicy raster{};
    if (classify_j3d_raster_policy(state, raster) != J3dRasterPolicyResult::Success)
        return J3dEffectMaterialResult::UnsupportedRasterPolicy;

    const Color constant = color_from_rgba8(state.konstColorRgba8[0]);
    const Color registerColor = color_from_s10(state.tevColorsS10[0]);
    material.texture = texture;
    material.textureCoordinates = stage.textureCoordinate == 1 ? ModelTextureCoordinates::Secondary
                                                               : ModelTextureCoordinates::Primary;
    material.modulation = {constant.r, constant.g, constant.b, registerColor.a};
    material.raster = raster;
    return J3dEffectMaterialResult::Success;
}

} // namespace sb::native_render
