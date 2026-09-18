#include <sunbright/native_render/j3d_masked_specular_material.h>

#include <sunbright/native_render/j3d_unlit_material.h>

#include <array>

namespace sb::native_render {
namespace {

constexpr std::uint16_t kSignedPrimaryDiffuse = 0x0686;
constexpr std::uint16_t kPrimaryLitMaterialAlpha = 0x0706;
constexpr std::uint16_t kDirectionalSpecular = 0x0212;
constexpr std::uint16_t kUnlitSecondaryAlpha = 0x0400;
constexpr std::uint8_t kColor0Alpha0 = 4;
constexpr std::uint8_t kColor1Alpha1 = 5;
// The detail stage's colour word selects the add-half bias, so the layer is offset by one half
// before it is clamped. The stage programs below pin that field, so the value is not a free choice.
constexpr float kAddHalfBias = 0.5F;
constexpr std::array<std::uint8_t, 8> kDetailOverDiffuseStage{0xC0, 0x09, 0xFA, 0xE8,
                                                              0xC1, 0x08, 0xFF, 0xD0};
constexpr std::array<std::uint8_t, 8> kMaskAgainstSpecularStage{0xC2, 0x08, 0xA0, 0x8F,
                                                                0xC3, 0x08, 0xFF, 0x80};
constexpr std::array<std::uint8_t, 8> kMaskAgainstSpecularAlphaStage{0xC2, 0x08, 0xA0, 0x8F,
                                                                     0xC3, 0x08, 0xF0, 0x70};

bool valid_texture(const PictureTexture& texture) noexcept {
    return texture.resource != 0 && texture.width != 0 && texture.height != 0;
}

} // namespace

const char* j3d_masked_specular_result_name(J3dMaskedSpecularResult result) noexcept {
    switch (result) {
    case J3dMaskedSpecularResult::Success:
        return "success";
    case J3dMaskedSpecularResult::UnsupportedColorBlock:
        return "unsupported colour block";
    case J3dMaskedSpecularResult::UnsupportedColorChannels:
        return "unsupported signed-diffuse/lit-alpha colour channels";
    case J3dMaskedSpecularResult::UnsupportedSecondaryColors:
        return "unsupported secondary material colours";
    case J3dMaskedSpecularResult::UnsupportedTevBlock:
        return "unsupported colour-stage block";
    case J3dMaskedSpecularResult::UnsupportedStageCount:
        return "unsupported colour-stage count";
    case J3dMaskedSpecularResult::MissingTextureCoordinates:
        return "missing texture coordinates";
    case J3dMaskedSpecularResult::UnsupportedTextureBindings:
        return "unsupported texture bindings";
    case J3dMaskedSpecularResult::UnsupportedColorProgram:
        return "unsupported mask/specular colour program";
    case J3dMaskedSpecularResult::MissingNormal:
        return "missing normal";
    case J3dMaskedSpecularResult::MissingLightingContext:
        return "missing lighting context";
    case J3dMaskedSpecularResult::UnsupportedRasterPolicy:
        return "unsupported raster policy";
    }
    return "unknown";
}

J3dMaskedSpecularResult classify_j3d_masked_specular_material(
    const J3dMaterialState& state, const PictureTexture& maskTexture,
    const PictureTexture& detailTexture, const ModelLightingContext& lighting,
    LitMaskedSpecularMaterial& material) noexcept {
    if (!state.supportedColorBlock) {
        return J3dMaskedSpecularResult::UnsupportedColorBlock;
    }
    if (!state.lightingEnabled || state.colorChannelCount != 2 ||
        state.colorChannelControl != kSignedPrimaryDiffuse ||
        state.alphaChannelControl != kPrimaryLitMaterialAlpha ||
        state.colorChannelControl1 != kDirectionalSpecular ||
        state.alphaChannelControl1 != kUnlitSecondaryAlpha) {
        return J3dMaskedSpecularResult::UnsupportedColorChannels;
    }
    if (state.ambientColor1Rgba8 != 0) {
        return J3dMaskedSpecularResult::UnsupportedSecondaryColors;
    }
    if (!state.supportedTevBlock) {
        return J3dMaskedSpecularResult::UnsupportedTevBlock;
    }
    if (state.tevStageCount != 2) {
        return J3dMaskedSpecularResult::UnsupportedStageCount;
    }
    if (state.textureCoordinateCount < 2) {
        return J3dMaskedSpecularResult::MissingTextureCoordinates;
    }
    // The detail stage reads map 1 through the diffuse channel; the mask stage reads map 0 through
    // the specular one. Slot 0 is therefore the mask image and slot 1 the detail image.
    if (state.textureBindings[0].textureNumber == 0xFFFFU ||
        state.textureBindings[1].textureNumber == 0xFFFFU ||
        state.tevStages[0].textureCoordinate != 1 || state.tevStages[0].textureMap != 1 ||
        state.tevStages[0].colorChannel != kColor0Alpha0 ||
        state.tevStages[1].textureCoordinate != 0 || state.tevStages[1].textureMap != 0 ||
        state.tevStages[1].colorChannel != kColor1Alpha1 || !valid_texture(maskTexture) ||
        !valid_texture(detailTexture)) {
        return J3dMaskedSpecularResult::UnsupportedTextureBindings;
    }
    const bool masksAlpha = state.tevStages[1].program == kMaskAgainstSpecularAlphaStage;
    if (state.tevStages[0].program != kDetailOverDiffuseStage ||
        (!masksAlpha && state.tevStages[1].program != kMaskAgainstSpecularStage) ||
        !selects(state.tevStages[0].konstColorSelection, J3dKonstFraction::FiveEighths)) {
        return J3dMaskedSpecularResult::UnsupportedColorProgram;
    }
    if (!state.hasNormal) {
        return J3dMaskedSpecularResult::MissingNormal;
    }
    if (!valid(lighting) || lighting.pointLightCount == 0) {
        return J3dMaskedSpecularResult::MissingLightingContext;
    }
    ModelRasterPolicy raster{};
    if (classify_j3d_raster_policy(state, raster) != J3dRasterPolicyResult::Success) {
        return J3dMaskedSpecularResult::UnsupportedRasterPolicy;
    }

    material.maskTexture = maskTexture;
    material.detailTexture = detailTexture;
    material.baseColor = color_from_rgba8(state.materialColorRgba8);
    material.ambientColor = state.usesMaterialAmbient ? color_from_rgba8(state.ambientColorRgba8)
                                                      : lighting.ambientColor;
    material.lighting = lighting;
    // The colour channel selects one light; the highlight rides in the lighting context with the
    // secondary material colour as its tint, as the other specular families publish it.
    material.lighting.pointLightCount = 1;
    tint_directional_specular(material.lighting, color_from_rgba8(state.materialColor1Rgba8));
    // The stage scales the lit colour by its selected constant and adds the detail image whole.
    material.diffuseWeight = value_of(J3dKonstFraction::FiveEighths);
    material.detailBias = kAddHalfBias;
    material.textureMasksAlpha = masksAlpha;
    material.raster = raster;
    return J3dMaskedSpecularResult::Success;
}

} // namespace sb::native_render
