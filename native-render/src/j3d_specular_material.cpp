#include <sunbright/native_render/j3d_specular_material.h>

#include <sunbright/native_render/j3d_unlit_material.h>

#include <array>
#include <cmath>

namespace sb::native_render {
namespace {

constexpr std::uint16_t kDiffuseColorChannel = 0x070E;
constexpr std::uint16_t kDiffuseVertexColorChannel = 0x070F;
constexpr std::uint16_t kMaterialAlphaChannel = 0x0700;
constexpr std::uint16_t kSpecularColorChannel = 0x0212;
constexpr std::uint16_t kUnlitSecondaryAlphaChannel = 0x0400;
constexpr std::uint8_t kColor0Alpha0 = 4;
constexpr std::uint8_t kColor1Alpha1 = 5;
constexpr std::uint8_t kKonstColor0 = 0x0C;
constexpr std::array<std::uint8_t, 8> kTintedTextureDiffuseStage{0xC0, 0x38, 0xF8, 0xAF,
                                                                 0xC1, 0x08, 0xF2, 0xF0};
constexpr std::array<std::uint8_t, 8> kTintSpecularStage{0xC2, 0x18, 0xEC, 0x0A,
                                                         0xC3, 0x00, 0xBF, 0xF1};
constexpr std::array<std::uint8_t, 8> kTripleSpecularStage{0xC0, 0x18, 0xFD, 0xAA,
                                                           0xC1, 0x08, 0xF2, 0xF0};
constexpr std::array<std::uint8_t, 8> kTextureDiffuseAddStage{0xC2, 0x08, 0xFA, 0x80,
                                                              0xC3, 0x00, 0xBF, 0xF0};

bool valid_lighting(const ModelLightingContext& lighting) noexcept {
    const DirectionalSpecularLight& specular = lighting.specular;
    return lighting.pointLightCount != 0 &&
           lighting.pointLightCount <= lighting.pointLights.size() &&
           std::isfinite(specular.directionToLight.x) &&
           std::isfinite(specular.directionToLight.y) &&
           std::isfinite(specular.directionToLight.z) && std::isfinite(specular.shininess) &&
           specular.shininess > 0.0F;
}

} // namespace

const char* j3d_specular_textured_result_name(J3dSpecularTexturedResult result) noexcept {
    switch (result) {
    case J3dSpecularTexturedResult::Success:
        return "success";
    case J3dSpecularTexturedResult::UnsupportedColorBlock:
        return "unsupported colour block";
    case J3dSpecularTexturedResult::UnsupportedColorChannels:
        return "unsupported diffuse/specular colour channels";
    case J3dSpecularTexturedResult::UnsupportedSecondaryColors:
        return "unsupported secondary material colours";
    case J3dSpecularTexturedResult::UnsupportedTevBlock:
        return "unsupported colour-stage block";
    case J3dSpecularTexturedResult::UnsupportedStageCount:
        return "unsupported colour-stage count";
    case J3dSpecularTexturedResult::MissingTextureCoordinate:
        return "missing texture coordinate";
    case J3dSpecularTexturedResult::UnsupportedTextureBinding:
        return "unsupported texture binding";
    case J3dSpecularTexturedResult::UnsupportedColorProgram:
        return "unsupported diffuse/specular colour program";
    case J3dSpecularTexturedResult::MissingNormal:
        return "missing normal";
    case J3dSpecularTexturedResult::MissingVertexColor:
        return "missing vertex colour";
    case J3dSpecularTexturedResult::MissingLightingContext:
        return "missing directional-specular lighting context";
    case J3dSpecularTexturedResult::UnsupportedRasterPolicy:
        return "unsupported raster policy";
    }
    return "unknown";
}

J3dSpecularTexturedResult classify_j3d_specular_textured_material(
    const J3dMaterialState& state, const PictureTexture& texture,
    const ModelLightingContext& lighting, LitSpecularTexturedMaterial& material) noexcept {
    if (!state.supportedColorBlock)
        return J3dSpecularTexturedResult::UnsupportedColorBlock;
    const bool tintedProgram = state.colorChannelControl == kDiffuseColorChannel;
    const bool tripleSpecularProgram = state.colorChannelControl == kDiffuseVertexColorChannel;
    if (!state.lightingEnabled || state.colorChannelCount != 2 ||
        (!tintedProgram && !tripleSpecularProgram) ||
        state.alphaChannelControl != kMaterialAlphaChannel ||
        state.colorChannelControl1 != kSpecularColorChannel ||
        state.alphaChannelControl1 != kUnlitSecondaryAlphaChannel) {
        return J3dSpecularTexturedResult::UnsupportedColorChannels;
    }
    if (state.materialColor1Rgba8 != 0xFFFFFFFFU || state.ambientColor1Rgba8 != 0)
        return J3dSpecularTexturedResult::UnsupportedSecondaryColors;
    if (!state.supportedTevBlock)
        return J3dSpecularTexturedResult::UnsupportedTevBlock;
    if (state.tevStageCount != 2)
        return J3dSpecularTexturedResult::UnsupportedStageCount;
    if (state.textureCoordinateCount == 0)
        return J3dSpecularTexturedResult::MissingTextureCoordinate;
    if (state.textureNumber0 == 0xFFFFU || state.textureNumber1 != 0xFFFFU ||
        state.textureCoordinate0 != 0 || state.textureMap0 != 0 ||
        (tintedProgram &&
         (state.colorChannel0 != kColor0Alpha0 || state.textureCoordinate1 != 0xFFU ||
          state.textureMap1 != 0xFFU || state.colorChannel1 != kColor1Alpha1)) ||
        (tripleSpecularProgram &&
         (state.colorChannel0 != kColor1Alpha1 || state.textureCoordinate1 != 0 ||
          state.textureMap1 != 0 || state.colorChannel1 != kColor0Alpha0)) ||
        texture.resource == 0 || texture.width == 0 || texture.height == 0) {
        return J3dSpecularTexturedResult::UnsupportedTextureBinding;
    }
    if ((tintedProgram &&
         (state.tevStage0 != kTintedTextureDiffuseStage || state.tevStage1 != kTintSpecularStage ||
          state.konstColorSelection0 != kKonstColor0 ||
          state.konstColorSelection1 != kKonstColor0)) ||
        (tripleSpecularProgram &&
         (state.tevStage0 != kTripleSpecularStage || state.tevStage1 != kTextureDiffuseAddStage))) {
        return J3dSpecularTexturedResult::UnsupportedColorProgram;
    }
    if (!state.hasNormal)
        return J3dSpecularTexturedResult::MissingNormal;
    if (tripleSpecularProgram && !state.hasVertexColor)
        return J3dSpecularTexturedResult::MissingVertexColor;
    if (!valid_lighting(lighting))
        return J3dSpecularTexturedResult::MissingLightingContext;
    ModelRasterPolicy raster{};
    if (classify_j3d_raster_policy(state, raster) != J3dRasterPolicyResult::Success)
        return J3dSpecularTexturedResult::UnsupportedRasterPolicy;

    material.texture = texture;
    material.baseColor = color_from_rgba8(state.materialColorRgba8);
    material.ambientColor = state.usesMaterialAmbient ? color_from_rgba8(state.ambientColorRgba8)
                                                      : lighting.ambientColor;
    const Color tint = color_from_rgba8(state.konstColorRgba8[0]);
    material.textureDiffuseScale = tintedProgram
                                       ? Color{1.0F - tint.r, 1.0F - tint.g, 1.0F - tint.b, 1.0F}
                                       : Color{1.0F, 1.0F, 1.0F, 1.0F};
    material.additiveColor =
        tintedProgram ? Color{2.0F * tint.r, 2.0F * tint.g, 2.0F * tint.b, 0.0F} : Color{};
    material.specularScale = tintedProgram ? 2.0F : 3.0F;
    material.lighting = lighting;
    material.usesVertexRgb = tripleSpecularProgram;
    material.raster = raster;
    return J3dSpecularTexturedResult::Success;
}

} // namespace sb::native_render
