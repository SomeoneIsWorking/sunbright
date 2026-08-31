#include <sunbright/native_render/j3d_lit_alpha_mask_material.h>

#include <sunbright/native_render/j3d_unlit_material.h>

#include <array>

namespace sb::native_render {
namespace {

constexpr std::uint16_t kDiffuseMaterialColor = 0x070E;
constexpr std::uint16_t kMaterialAlpha = 0x0700;
constexpr std::uint8_t kColor0Alpha0 = 4;
constexpr std::uint8_t kColorNull = 0xFF;
constexpr std::array<std::uint8_t, 8> kTextureTimesLitColor{0xC0, 0x08, 0xF8, 0xAF,
                                                            0xC1, 0x08, 0xFF, 0xC0};
constexpr std::array<std::uint8_t, 8> kKeepRgbApplyScaledMaskAlpha{0xC2, 0x08, 0xFF, 0xF0,
                                                                   0xC3, 0x28, 0xF0, 0xF0};
constexpr std::array<std::int16_t, 4> kBlackOpaqueRegister{0, 0, 0, 255};

bool valid_texture(const PictureTexture& texture) noexcept {
    return texture.resource != 0 && texture.width != 0 && texture.height != 0;
}

} // namespace

const char* j3d_lit_alpha_mask_result_name(J3dLitAlphaMaskResult result) noexcept {
    switch (result) {
    case J3dLitAlphaMaskResult::Success:
        return "success";
    case J3dLitAlphaMaskResult::UnsupportedColorBlock:
        return "unsupported colour block";
    case J3dLitAlphaMaskResult::UnsupportedColorChannels:
        return "unsupported diffuse colour channels";
    case J3dLitAlphaMaskResult::UnsupportedTevBlock:
        return "unsupported colour-stage block";
    case J3dLitAlphaMaskResult::UnsupportedStageCount:
        return "unsupported colour-stage count";
    case J3dLitAlphaMaskResult::MissingTextureCoordinates:
        return "missing second texture coordinate";
    case J3dLitAlphaMaskResult::UnsupportedTextureBindings:
        return "unsupported colour/mask texture bindings";
    case J3dLitAlphaMaskResult::UnsupportedColorProgram:
        return "unsupported lit alpha-mask program";
    case J3dLitAlphaMaskResult::UnsupportedRegisterColor:
        return "unsupported alpha-scale register";
    case J3dLitAlphaMaskResult::MissingNormal:
        return "missing normal";
    case J3dLitAlphaMaskResult::MissingLightingContext:
        return "missing lighting context";
    case J3dLitAlphaMaskResult::UnsupportedRasterPolicy:
        return "unsupported raster policy";
    }
    return "unknown";
}

J3dLitAlphaMaskResult classify_j3d_lit_alpha_mask_material(
    const J3dMaterialState& state, const PictureTexture& colorTexture,
    const PictureTexture& alphaMaskTexture, const ModelLightingContext& lighting,
    LitTexturedAlphaMaskMaterial& material) noexcept {
    if (!state.supportedColorBlock)
        return J3dLitAlphaMaskResult::UnsupportedColorBlock;
    if (!state.lightingEnabled || state.colorChannelCount != 1 ||
        state.colorChannelControl != kDiffuseMaterialColor ||
        state.alphaChannelControl != kMaterialAlpha) {
        return J3dLitAlphaMaskResult::UnsupportedColorChannels;
    }
    if (!state.supportedTevBlock)
        return J3dLitAlphaMaskResult::UnsupportedTevBlock;
    if (state.tevStageCount != 2)
        return J3dLitAlphaMaskResult::UnsupportedStageCount;
    if (state.textureCoordinateCount < 2)
        return J3dLitAlphaMaskResult::MissingTextureCoordinates;
    if (state.textureNumber0 == 0xFFFFU || state.textureNumber1 == 0xFFFFU ||
        state.textureCoordinate0 != 0 || state.textureMap0 != 0 ||
        state.colorChannel0 != kColor0Alpha0 || state.textureCoordinate1 != 1 ||
        state.textureMap1 != 1 || state.colorChannel1 != kColorNull ||
        !valid_texture(colorTexture) || !valid_texture(alphaMaskTexture)) {
        return J3dLitAlphaMaskResult::UnsupportedTextureBindings;
    }
    if (state.tevStage0 != kTextureTimesLitColor ||
        state.tevStage1 != kKeepRgbApplyScaledMaskAlpha) {
        return J3dLitAlphaMaskResult::UnsupportedColorProgram;
    }
    if (!state.hasTevColor0 || state.tevColor0S10 != kBlackOpaqueRegister)
        return J3dLitAlphaMaskResult::UnsupportedRegisterColor;
    if (!state.hasNormal)
        return J3dLitAlphaMaskResult::MissingNormal;
    if (lighting.pointLightCount == 0 || lighting.pointLightCount > lighting.pointLights.size())
        return J3dLitAlphaMaskResult::MissingLightingContext;
    ModelRasterPolicy raster{};
    if (classify_j3d_raster_policy(state, raster) != J3dRasterPolicyResult::Success)
        return J3dLitAlphaMaskResult::UnsupportedRasterPolicy;

    material.colorTexture = colorTexture;
    material.alphaMaskTexture = alphaMaskTexture;
    material.baseColor = color_from_rgba8(state.materialColorRgba8);
    material.ambientColor = state.usesMaterialAmbient ? color_from_rgba8(state.ambientColorRgba8)
                                                      : lighting.ambientColor;
    material.lighting = lighting;
    material.alphaScale = 4.0F;
    material.raster = raster;
    return J3dLitAlphaMaskResult::Success;
}

} // namespace sb::native_render
