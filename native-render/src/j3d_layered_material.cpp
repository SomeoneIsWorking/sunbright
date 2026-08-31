#include <sunbright/native_render/j3d_layered_material.h>

#include <sunbright/native_render/j3d_unlit_material.h>

#include <array>

namespace sb::native_render {
namespace {

constexpr std::uint16_t kSignedPrimaryDiffuse = 0x0686;
constexpr std::uint16_t kPrimaryLitMaterialAlpha = 0x0706;
constexpr std::uint8_t kColor0Alpha0 = 4;
constexpr std::uint8_t kColorNull = 0xFF;
constexpr std::uint8_t kFiveEighths = 0x03;
constexpr std::array<std::uint8_t, 8> kBlendDetailAndLitColor{0xC0, 0x08, 0x8A, 0xEF,
                                                              0xC1, 0x08, 0xFF, 0xD0};
constexpr std::array<std::uint8_t, 8> kMultiplyBaseTexture{0xC2, 0x08, 0xF0, 0x8F,
                                                           0xC3, 0x08, 0xF0, 0x70};

bool valid_texture(const PictureTexture& texture) noexcept {
    return texture.resource != 0 && texture.width != 0 && texture.height != 0;
}

bool supported_primary_light(const ModelLightingContext& lighting) noexcept {
    if (lighting.pointLightCount == 0 || lighting.pointLightCount > lighting.pointLights.size())
        return false;
    const PointLight& primary = lighting.pointLights[0];
    return primary.color.a == 1.0F && primary.distanceAttenuation.x == 1.0F &&
           primary.distanceAttenuation.y == 0.0F && primary.distanceAttenuation.z == 0.0F;
}

} // namespace

const char* j3d_layered_material_result_name(J3dLayeredMaterialResult result) noexcept {
    switch (result) {
    case J3dLayeredMaterialResult::Success:
        return "success";
    case J3dLayeredMaterialResult::UnsupportedColorBlock:
        return "unsupported colour block";
    case J3dLayeredMaterialResult::UnsupportedColorChannels:
        return "unsupported signed-diffuse colour channels";
    case J3dLayeredMaterialResult::UnsupportedTevBlock:
        return "unsupported colour-stage block";
    case J3dLayeredMaterialResult::UnsupportedStageCount:
        return "unsupported colour-stage count";
    case J3dLayeredMaterialResult::MissingTextureCoordinates:
        return "missing layered texture coordinates";
    case J3dLayeredMaterialResult::UnsupportedTextureBindings:
        return "unsupported layered texture bindings";
    case J3dLayeredMaterialResult::UnsupportedColorProgram:
        return "unsupported weighted layered colour program";
    case J3dLayeredMaterialResult::MissingNormal:
        return "missing normal";
    case J3dLayeredMaterialResult::MissingLightingContext:
        return "missing constant-attenuation primary light";
    case J3dLayeredMaterialResult::UnsupportedRasterPolicy:
        return "unsupported raster policy";
    }
    return "unknown";
}

J3dLayeredMaterialResult
classify_j3d_layered_material(const J3dMaterialState& state, const PictureTexture& baseTexture,
                              const PictureTexture& detailTexture,
                              const ModelLightingContext& lighting,
                              LitLayeredTexturedMaterial& material) noexcept {
    if (!state.supportedColorBlock)
        return J3dLayeredMaterialResult::UnsupportedColorBlock;
    if (!state.lightingEnabled || state.colorChannelCount != 1 ||
        state.colorChannelControl != kSignedPrimaryDiffuse ||
        state.alphaChannelControl != kPrimaryLitMaterialAlpha) {
        return J3dLayeredMaterialResult::UnsupportedColorChannels;
    }
    if (!state.supportedTevBlock)
        return J3dLayeredMaterialResult::UnsupportedTevBlock;
    if (state.tevStageCount != 2)
        return J3dLayeredMaterialResult::UnsupportedStageCount;
    if (state.textureCoordinateCount < 2)
        return J3dLayeredMaterialResult::MissingTextureCoordinates;
    if (state.textureBindings[0].textureNumber == 0xFFFFU ||
        state.textureBindings[1].textureNumber == 0xFFFFU ||
        state.tevStages[0].textureCoordinate != 1 || state.tevStages[0].textureMap != 1 ||
        state.tevStages[0].colorChannel != kColor0Alpha0 ||
        state.tevStages[1].textureCoordinate != 0 || state.tevStages[1].textureMap != 0 ||
        state.tevStages[1].colorChannel != kColorNull || !valid_texture(baseTexture) ||
        !valid_texture(detailTexture)) {
        return J3dLayeredMaterialResult::UnsupportedTextureBindings;
    }
    if (state.tevStages[0].program != kBlendDetailAndLitColor ||
        state.tevStages[1].program != kMultiplyBaseTexture ||
        state.tevStages[0].konstColorSelection != kFiveEighths) {
        return J3dLayeredMaterialResult::UnsupportedColorProgram;
    }
    if (!state.hasNormal)
        return J3dLayeredMaterialResult::MissingNormal;
    if (!supported_primary_light(lighting))
        return J3dLayeredMaterialResult::MissingLightingContext;
    ModelRasterPolicy raster{};
    if (classify_j3d_raster_policy(state, raster) != J3dRasterPolicyResult::Success)
        return J3dLayeredMaterialResult::UnsupportedRasterPolicy;

    material.baseTexture = baseTexture;
    material.detailTexture = detailTexture;
    material.baseColor = color_from_rgba8(state.materialColorRgba8);
    material.ambientColor = state.usesMaterialAmbient ? color_from_rgba8(state.ambientColorRgba8)
                                                      : lighting.ambientColor;
    material.lighting = lighting;
    material.lighting.pointLightCount = 1;
    material.detailWeight = 3.0F / 8.0F;
    material.diffuseMode = ModelDiffuseMode::Signed;
    material.raster = raster;
    return J3dLayeredMaterialResult::Success;
}
} // namespace sb::native_render
