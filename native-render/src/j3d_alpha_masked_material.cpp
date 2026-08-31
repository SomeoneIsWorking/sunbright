#include <sunbright/native_render/j3d_alpha_masked_material.h>

#include <sunbright/native_render/j3d_unlit_material.h>

#include <array>

namespace sb::native_render {
namespace {

constexpr std::uint16_t kDiffuseMaterialColor = 0x070E;
constexpr std::uint16_t kMaterialAlpha = 0x0700;
constexpr std::uint8_t kColor0Alpha0 = 4;
constexpr std::array<std::uint8_t, 8> kBlackRgbTextureAlphaStage{0xC0, 0x08, 0xF2, 0xAF,
                                                                 0xC1, 0x28, 0xF0, 0xF0};
constexpr std::array<std::int16_t, 4> kBlackOpaqueRegister{0, 0, 0, 255};

} // namespace

const char* j3d_alpha_masked_material_result_name(J3dAlphaMaskedMaterialResult result) noexcept {
    switch (result) {
    case J3dAlphaMaskedMaterialResult::Success:
        return "success";
    case J3dAlphaMaskedMaterialResult::UnsupportedColorBlock:
        return "unsupported colour block";
    case J3dAlphaMaskedMaterialResult::UnsupportedColorChannels:
        return "unsupported source colour channels";
    case J3dAlphaMaskedMaterialResult::UnsupportedTevBlock:
        return "unsupported colour-stage block";
    case J3dAlphaMaskedMaterialResult::UnsupportedStageCount:
        return "unsupported colour-stage count";
    case J3dAlphaMaskedMaterialResult::MissingTextureCoordinate:
        return "missing texture coordinate";
    case J3dAlphaMaskedMaterialResult::UnsupportedTextureBinding:
        return "unsupported mask texture binding";
    case J3dAlphaMaskedMaterialResult::UnsupportedColorProgram:
        return "unsupported solid-colour mask program";
    case J3dAlphaMaskedMaterialResult::UnsupportedRegisterColor:
        return "unsupported solid-colour register";
    case J3dAlphaMaskedMaterialResult::MissingNormal:
        return "missing source normal";
    case J3dAlphaMaskedMaterialResult::UnsupportedRasterPolicy:
        return "unsupported raster policy";
    }
    return "unknown";
}

J3dAlphaMaskedMaterialResult
classify_j3d_alpha_masked_material(const J3dMaterialState& state, const PictureTexture& texture,
                                   AlphaMaskedColorMaterial& material) noexcept {
    if (!state.supportedColorBlock)
        return J3dAlphaMaskedMaterialResult::UnsupportedColorBlock;
    if (!state.lightingEnabled || state.colorChannelCount != 1 ||
        state.colorChannelControl != kDiffuseMaterialColor ||
        state.alphaChannelControl != kMaterialAlpha) {
        return J3dAlphaMaskedMaterialResult::UnsupportedColorChannels;
    }
    if (!state.supportedTevBlock)
        return J3dAlphaMaskedMaterialResult::UnsupportedTevBlock;
    if (state.tevStageCount != 1)
        return J3dAlphaMaskedMaterialResult::UnsupportedStageCount;
    if (state.textureCoordinateCount == 0)
        return J3dAlphaMaskedMaterialResult::MissingTextureCoordinate;
    if (state.textureBindings[0].textureNumber == 0xFFFFU ||
        state.tevStages[0].textureCoordinate != 0 || state.tevStages[0].textureMap != 0 ||
        state.tevStages[0].colorChannel != kColor0Alpha0 || texture.resource == 0 ||
        texture.width == 0 || texture.height == 0) {
        return J3dAlphaMaskedMaterialResult::UnsupportedTextureBinding;
    }
    if (state.tevStages[0].program != kBlackRgbTextureAlphaStage)
        return J3dAlphaMaskedMaterialResult::UnsupportedColorProgram;
    if (!state.hasTevColor0 || state.tevColor0S10 != kBlackOpaqueRegister)
        return J3dAlphaMaskedMaterialResult::UnsupportedRegisterColor;
    if (!state.hasNormal)
        return J3dAlphaMaskedMaterialResult::MissingNormal;
    ModelRasterPolicy raster{};
    if (classify_j3d_raster_policy(state, raster) != J3dRasterPolicyResult::Success)
        return J3dAlphaMaskedMaterialResult::UnsupportedRasterPolicy;

    material.texture = texture;
    material.color = {0.0F, 0.0F, 0.0F, 1.0F};
    material.alphaScale = 4.0F;
    material.raster = raster;
    return J3dAlphaMaskedMaterialResult::Success;
}

} // namespace sb::native_render
