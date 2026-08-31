#include <sunbright/native_render/j3d_tinted_layered_material.h>

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
constexpr std::uint8_t kFiveEighths = 0x03;
constexpr std::uint8_t kHalf = 0x04;
constexpr std::array<std::uint8_t, 8> kTintDetailDiffuseStage{0xC0, 0x0A, 0x8A, 0xE2,
                                                              0xC1, 0x08, 0xFF, 0xD0};
constexpr std::array<std::uint8_t, 8> kBaseLayerSpecularStage{0xC2, 0x0A, 0x0A, 0xE8,
                                                              0xC3, 0x08, 0xFF, 0x80};

bool valid_texture(const PictureTexture& texture) noexcept {
    return texture.resource != 0 && texture.width != 0 && texture.height != 0;
}

Color color_from_s10(const std::array<std::int16_t, 4>& color) noexcept {
    constexpr float kScale = 1.0F / 255.0F;
    return {color[0] * kScale, color[1] * kScale, color[2] * kScale, color[3] * kScale};
}

} // namespace

const char*
j3d_tinted_layered_material_result_name(J3dTintedLayeredMaterialResult result) noexcept {
    switch (result) {
    case J3dTintedLayeredMaterialResult::Success:
        return "success";
    case J3dTintedLayeredMaterialResult::UnsupportedColorBlock:
        return "unsupported colour block";
    case J3dTintedLayeredMaterialResult::UnsupportedColorChannels:
        return "unsupported signed-diffuse/specular colour channels";
    case J3dTintedLayeredMaterialResult::UnsupportedSecondaryColors:
        return "unsupported secondary material colours";
    case J3dTintedLayeredMaterialResult::UnsupportedTevBlock:
        return "unsupported colour-stage block";
    case J3dTintedLayeredMaterialResult::UnsupportedStageCount:
        return "unsupported colour-stage count";
    case J3dTintedLayeredMaterialResult::MissingTextureCoordinates:
        return "missing layered texture coordinates";
    case J3dTintedLayeredMaterialResult::UnsupportedTextureBindings:
        return "unsupported layered texture bindings";
    case J3dTintedLayeredMaterialResult::UnsupportedColorProgram:
        return "unsupported tinted layered colour program";
    case J3dTintedLayeredMaterialResult::MissingNormal:
        return "missing normal";
    case J3dTintedLayeredMaterialResult::MissingLightingContext:
        return "missing two-light diffuse/specular context";
    case J3dTintedLayeredMaterialResult::UnsupportedRasterPolicy:
        return "unsupported raster policy";
    }
    return "unknown";
}

J3dTintedLayeredMaterialResult classify_j3d_tinted_layered_material(
    const J3dMaterialState& state, const PictureTexture& baseTexture,
    const PictureTexture& detailTexture, const ModelLightingContext& lighting,
    LitTintedLayeredSpecularMaterial& material) noexcept {
    if (!state.supportedColorBlock)
        return J3dTintedLayeredMaterialResult::UnsupportedColorBlock;
    if (!state.lightingEnabled || state.colorChannelCount != 2 ||
        state.colorChannelControl != kTwoLightSignedDiffuse ||
        state.alphaChannelControl != kMaterialAlpha ||
        state.colorChannelControl1 != kDirectionalSpecular ||
        state.alphaChannelControl1 != kUnlitSecondaryAlpha) {
        return J3dTintedLayeredMaterialResult::UnsupportedColorChannels;
    }
    if (state.ambientColor1Rgba8 != 0)
        return J3dTintedLayeredMaterialResult::UnsupportedSecondaryColors;
    if (!state.supportedTevBlock)
        return J3dTintedLayeredMaterialResult::UnsupportedTevBlock;
    if (state.tevStageCount != 2)
        return J3dTintedLayeredMaterialResult::UnsupportedStageCount;
    if (state.textureCoordinateCount < 2)
        return J3dTintedLayeredMaterialResult::MissingTextureCoordinates;
    if (state.textureBindings[0].textureNumber == 0xFFFFU ||
        state.textureBindings[1].textureNumber == 0xFFFFU ||
        state.tevStages[0].textureCoordinate != 1 || state.tevStages[0].textureMap != 1 ||
        state.tevStages[0].colorChannel != kColor0Alpha0 ||
        state.tevStages[1].textureCoordinate != 0 || state.tevStages[1].textureMap != 0 ||
        state.tevStages[1].colorChannel != kColor1Alpha1 || !valid_texture(baseTexture) ||
        !valid_texture(detailTexture)) {
        return J3dTintedLayeredMaterialResult::UnsupportedTextureBindings;
    }
    if (state.tevStages[0].program != kTintDetailDiffuseStage ||
        state.tevStages[1].program != kBaseLayerSpecularStage ||
        state.tevStages[0].konstColorSelection != kFiveEighths ||
        state.tevStages[1].konstColorSelection != kHalf) {
        return J3dTintedLayeredMaterialResult::UnsupportedColorProgram;
    }
    if (!state.hasNormal)
        return J3dTintedLayeredMaterialResult::MissingNormal;
    if (!valid(lighting) || lighting.pointLightCount == 0)
        return J3dTintedLayeredMaterialResult::MissingLightingContext;
    ModelRasterPolicy raster{};
    if (classify_j3d_raster_policy(state, raster) != J3dRasterPolicyResult::Success)
        return J3dTintedLayeredMaterialResult::UnsupportedRasterPolicy;

    material.baseTexture = baseTexture;
    material.detailTexture = detailTexture;
    material.baseColor = color_from_rgba8(state.materialColorRgba8);
    material.ambientColor = state.usesMaterialAmbient ? color_from_rgba8(state.ambientColorRgba8)
                                                      : lighting.ambientColor;
    material.effectColor = color_from_s10(state.tevColorsS10[0]);
    material.lighting = lighting;
    tint_directional_specular(material.lighting, color_from_rgba8(state.materialColor1Rgba8));
    material.detailWeight = 3.0F / 8.0F;
    material.layerWeight = 0.5F;
    material.raster = raster;
    return J3dTintedLayeredMaterialResult::Success;
}

} // namespace sb::native_render
