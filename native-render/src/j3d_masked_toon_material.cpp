#include <sunbright/native_render/j3d_masked_toon_material.h>

#include <sunbright/native_render/j3d_unlit_material.h>

#include <array>

namespace sb::native_render {
namespace {

constexpr std::uint16_t kTwoLightSignedDiffuse = 0x068E;
constexpr std::uint16_t kMaterialAlpha = 0x0700;
constexpr std::uint16_t kDirectionalSpecular = 0x0212;
constexpr std::uint16_t kUnlitSecondaryAlpha = 0x0400;
constexpr std::uint8_t kColor0Alpha0 = 4;
constexpr std::uint8_t kColor1Alpha1 = 5;
constexpr std::uint8_t kNoTexture = 0xFF;
constexpr std::uint8_t kThreeEighths = 0x05;
constexpr std::uint8_t kFiveEighths = 0x03;
constexpr std::uint8_t kHalf = 0x04;
constexpr std::uint8_t kKonstAlpha0 = 0x1C;
constexpr std::array<std::uint8_t, 8> kMaskCompareStage{0xC0, 0xDB, 0x9E, 0xCF,
                                                        0xC1, 0x08, 0xFF, 0xF0};
constexpr std::array<std::uint8_t, 8> kPrimaryMaskedStage{0xC2, 0x08, 0xF8, 0x6F,
                                                          0xC3, 0x08, 0xFF, 0xF0};
constexpr std::array<std::uint8_t, 8> kAlternateMaskedStage{0xC4, 0x08, 0x8F, 0x60,
                                                            0xC5, 0x08, 0xFF, 0xF0};
constexpr std::array<std::uint8_t, 8> kLightRampStage{0xC6, 0x8A, 0x8A, 0xE2,
                                                      0xC7, 0x00, 0xFF, 0xF0};
constexpr std::array<std::uint8_t, 8> kHighlightStage{0xC8, 0x0A, 0x4A, 0xE0,
                                                      0xC9, 0x00, 0xFF, 0xD0};

bool valid_texture(const PictureTexture& texture) noexcept {
    return texture.resource != 0 && texture.width != 0 && texture.height != 0;
}

Color color_from_s10(const std::array<std::int16_t, 4>& color) noexcept {
    constexpr float kScale = 1.0F / 255.0F;
    return {color[0] * kScale, color[1] * kScale, color[2] * kScale, color[3] * kScale};
}

} // namespace

const char* j3d_masked_toon_material_result_name(J3dMaskedToonMaterialResult result) noexcept {
    switch (result) {
    case J3dMaskedToonMaterialResult::Success:
        return "success";
    case J3dMaskedToonMaterialResult::UnsupportedColorBlock:
        return "unsupported colour block";
    case J3dMaskedToonMaterialResult::UnsupportedColorChannels:
        return "unsupported signed-diffuse/specular colour channels";
    case J3dMaskedToonMaterialResult::UnsupportedSecondaryColors:
        return "unsupported secondary material colours";
    case J3dMaskedToonMaterialResult::UnsupportedTevBlock:
        return "unsupported colour-stage block";
    case J3dMaskedToonMaterialResult::UnsupportedStageCount:
        return "unsupported colour-stage count";
    case J3dMaskedToonMaterialResult::MissingTextureCoordinates:
        return "missing four texture coordinates";
    case J3dMaskedToonMaterialResult::UnsupportedTextureBindings:
        return "unsupported masked-toon texture bindings";
    case J3dMaskedToonMaterialResult::UnsupportedColorProgram:
        return "unsupported masked-toon colour program";
    case J3dMaskedToonMaterialResult::MissingRegisterColor:
        return "missing masked-toon highlight register colour";
    case J3dMaskedToonMaterialResult::MissingNormal:
        return "missing normal";
    case J3dMaskedToonMaterialResult::MissingLightingContext:
        return "missing diffuse/specular lighting context";
    case J3dMaskedToonMaterialResult::UnsupportedRasterPolicy:
        return "unsupported raster policy";
    case J3dMaskedToonMaterialResult::MissingTextureBinding:
        return "missing masked-toon texture binding";
    case J3dMaskedToonMaterialResult::InvalidTextureResource:
        return "invalid masked-toon texture resource";
    }
    return "unknown";
}

J3dMaskedToonMaterialResult classify_j3d_masked_toon_material(
    const J3dMaterialState& state, const PictureTexture& primaryTexture,
    const PictureTexture& maskTexture, const PictureTexture& alternateTexture,
    const PictureTexture& lightRampTexture, const ModelLightingContext& lighting,
    LitMaskedToonMaterial& material) noexcept {
    if (!state.supportedColorBlock)
        return J3dMaskedToonMaterialResult::UnsupportedColorBlock;
    if (!state.lightingEnabled || state.colorChannelCount != 2 ||
        state.colorChannelControl != kTwoLightSignedDiffuse ||
        state.alphaChannelControl != kMaterialAlpha ||
        state.colorChannelControl1 != kDirectionalSpecular ||
        state.alphaChannelControl1 != kUnlitSecondaryAlpha) {
        return J3dMaskedToonMaterialResult::UnsupportedColorChannels;
    }
    if (state.ambientColor1Rgba8 != 0)
        return J3dMaskedToonMaterialResult::UnsupportedSecondaryColors;
    if (!state.supportedTevBlock)
        return J3dMaskedToonMaterialResult::UnsupportedTevBlock;
    if (state.tevStageCount != 5)
        return J3dMaskedToonMaterialResult::UnsupportedStageCount;
    if (state.textureCoordinateCount < 4)
        return J3dMaskedToonMaterialResult::MissingTextureCoordinates;
    const auto texture_bound = [&](std::size_t index) {
        return state.textureBindings[index].textureNumber != 0xFFFFU;
    };
    if (!texture_bound(0) || !texture_bound(1) || !texture_bound(2) || !texture_bound(3))
        return J3dMaskedToonMaterialResult::MissingTextureBinding;
    if (state.tevStages[0].textureCoordinate != 1 || state.tevStages[0].textureMap != 1 ||
        state.tevStages[0].colorChannel != kNoTexture ||
        state.tevStages[1].textureCoordinate != 0 || state.tevStages[1].textureMap != 0 ||
        state.tevStages[1].colorChannel != kNoTexture ||
        state.tevStages[2].textureCoordinate != 2 || state.tevStages[2].textureMap != 2 ||
        state.tevStages[2].colorChannel != kNoTexture ||
        state.tevStages[3].textureCoordinate != 3 || state.tevStages[3].textureMap != 3 ||
        state.tevStages[3].colorChannel != kColor0Alpha0 ||
        state.tevStages[4].textureCoordinate != kNoTexture ||
        state.tevStages[4].textureMap != kNoTexture ||
        state.tevStages[4].colorChannel != kColor1Alpha1) {
        return J3dMaskedToonMaterialResult::UnsupportedTextureBindings;
    }
    if (!valid_texture(primaryTexture) || !valid_texture(maskTexture) ||
        !valid_texture(alternateTexture) || !valid_texture(lightRampTexture))
        return J3dMaskedToonMaterialResult::InvalidTextureResource;
    if (state.tevStages[0].program != kMaskCompareStage ||
        state.tevStages[1].program != kPrimaryMaskedStage ||
        state.tevStages[2].program != kAlternateMaskedStage ||
        state.tevStages[3].program != kLightRampStage ||
        state.tevStages[4].program != kHighlightStage ||
        state.tevStages[0].konstColorSelection != kKonstAlpha0 ||
        state.tevStages[0].konstAlphaSelection != kKonstAlpha0 ||
        state.tevStages[1].konstColorSelection != kFiveEighths ||
        state.tevStages[1].konstAlphaSelection != 0 ||
        state.tevStages[2].konstColorSelection != kThreeEighths ||
        state.tevStages[2].konstAlphaSelection != kKonstAlpha0 ||
        state.tevStages[3].konstColorSelection != kFiveEighths ||
        state.tevStages[3].konstAlphaSelection != kKonstAlpha0 ||
        state.tevStages[4].konstColorSelection != kHalf ||
        state.tevStages[4].konstAlphaSelection != kKonstAlpha0) {
        return J3dMaskedToonMaterialResult::UnsupportedColorProgram;
    }
    if (!state.hasTevColors)
        return J3dMaskedToonMaterialResult::MissingRegisterColor;
    if (!state.hasNormal)
        return J3dMaskedToonMaterialResult::MissingNormal;
    if (!valid(lighting) || lighting.pointLightCount == 0)
        return J3dMaskedToonMaterialResult::MissingLightingContext;
    ModelRasterPolicy raster{};
    if (classify_j3d_raster_policy(state, raster) != J3dRasterPolicyResult::Success)
        return J3dMaskedToonMaterialResult::UnsupportedRasterPolicy;

    material.primaryTexture = primaryTexture;
    material.maskTexture = maskTexture;
    material.alternateTexture = alternateTexture;
    material.lightRampTexture = lightRampTexture;
    material.baseColor = color_from_rgba8(state.materialColorRgba8);
    material.ambientColor = state.usesMaterialAmbient ? color_from_rgba8(state.ambientColorRgba8)
                                                      : lighting.ambientColor;
    material.staticHighlight = color_from_s10(state.tevColorsS10[1]);
    material.lighting = lighting;
    const Color secondaryMaterialColor = color_from_rgba8(state.materialColor1Rgba8);
    tint_directional_specular(material.lighting, secondaryMaterialColor);
    material.lightRampWeight = 3.0F / 8.0F;
    material.staticHighlightWeight = 0.5F;
    material.directionalHighlightWeight = 0.5F;
    material.outputAlpha = secondaryMaterialColor.a;
    material.raster = raster;
    return J3dMaskedToonMaterialResult::Success;
}

} // namespace sb::native_render
