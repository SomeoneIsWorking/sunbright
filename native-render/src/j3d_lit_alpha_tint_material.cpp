#include <sunbright/native_render/j3d_lit_alpha_tint_material.h>

#include <sunbright/native_render/j3d_unlit_material.h>

#include <array>

namespace sb::native_render {
namespace {

constexpr std::uint16_t kPrimaryLightDiffuseChannel = 0x0706;
constexpr std::uint16_t kMaterialAlphaChannel = 0x0700;
constexpr std::uint8_t kColor0Alpha0 = 4;
constexpr std::array<std::uint8_t, 8> kRasterTimesRegisterWithTextureAlpha{0xC0, 0x08, 0xF8, 0x2F,
                                                                           0xC1, 0x08, 0xF2, 0xF0};

Color color_from_s10_rgb(const std::array<std::int16_t, 4>& color) noexcept {
    constexpr float kScale = 1.0F / 255.0F;
    return {color[0] * kScale, color[1] * kScale, color[2] * kScale, 1.0F};
}

bool valid_lighting(const ModelLightingContext& lighting) noexcept {
    return lighting.pointLightCount != 0 && lighting.pointLightCount <= lighting.pointLights.size();
}

} // namespace

const char* j3d_lit_alpha_tint_result_name(J3dLitAlphaTintResult result) noexcept {
    switch (result) {
    case J3dLitAlphaTintResult::Success:
        return "success";
    case J3dLitAlphaTintResult::UnsupportedColorBlock:
        return "unsupported colour block";
    case J3dLitAlphaTintResult::UnsupportedColorChannels:
        return "unsupported diffuse colour channels";
    case J3dLitAlphaTintResult::UnsupportedTevBlock:
        return "unsupported colour-stage block";
    case J3dLitAlphaTintResult::UnsupportedStageCount:
        return "unsupported colour-stage count";
    case J3dLitAlphaTintResult::MissingTextureCoordinate:
        return "missing texture coordinate";
    case J3dLitAlphaTintResult::UnsupportedTextureBinding:
        return "unsupported alpha texture binding";
    case J3dLitAlphaTintResult::UnsupportedColorProgram:
        return "unsupported lit alpha-tint program";
    case J3dLitAlphaTintResult::MissingTevColor:
        return "missing lit alpha-tint colour";
    case J3dLitAlphaTintResult::MissingNormal:
        return "missing normal";
    case J3dLitAlphaTintResult::MissingLightingContext:
        return "missing lighting context";
    case J3dLitAlphaTintResult::UnsupportedRasterPolicy:
        return "unsupported raster policy";
    }
    return "unknown";
}

J3dLitAlphaTintResult
classify_j3d_lit_alpha_tint_material(const J3dMaterialState& state, const PictureTexture& texture,
                                     const ModelLightingContext& lighting,
                                     LitAlphaTintMaterial& material) noexcept {
    if (!state.supportedColorBlock)
        return J3dLitAlphaTintResult::UnsupportedColorBlock;
    if (!state.lightingEnabled || state.colorChannelCount != 1 ||
        state.colorChannelControl != kPrimaryLightDiffuseChannel ||
        state.alphaChannelControl != kMaterialAlphaChannel)
        return J3dLitAlphaTintResult::UnsupportedColorChannels;
    if (!state.supportedTevBlock)
        return J3dLitAlphaTintResult::UnsupportedTevBlock;
    if (state.tevStageCount != 1)
        return J3dLitAlphaTintResult::UnsupportedStageCount;
    if (state.textureCoordinateCount == 0)
        return J3dLitAlphaTintResult::MissingTextureCoordinate;
    const J3dTevStageState& stage = state.tevStages[0];
    if (state.textureBindings[0].textureNumber == 0xFFFFU || stage.textureCoordinate != 0 ||
        stage.textureMap != 0 || stage.colorChannel != kColor0Alpha0 || texture.resource == 0 ||
        texture.width == 0 || texture.height == 0)
        return J3dLitAlphaTintResult::UnsupportedTextureBinding;
    if (stage.program != kRasterTimesRegisterWithTextureAlpha)
        return J3dLitAlphaTintResult::UnsupportedColorProgram;
    if (!state.hasTevColors)
        return J3dLitAlphaTintResult::MissingTevColor;
    if (!state.hasNormal)
        return J3dLitAlphaTintResult::MissingNormal;
    if (!valid_lighting(lighting))
        return J3dLitAlphaTintResult::MissingLightingContext;
    ModelRasterPolicy raster{};
    if (classify_j3d_raster_policy(state, raster) != J3dRasterPolicyResult::Success)
        return J3dLitAlphaTintResult::UnsupportedRasterPolicy;

    material.texture = texture;
    material.tint = color_from_s10_rgb(state.tevColorsS10[0]);
    material.baseColor = color_from_rgba8(state.materialColorRgba8);
    material.ambientColor = state.usesMaterialAmbient ? color_from_rgba8(state.ambientColorRgba8)
                                                      : lighting.ambientColor;
    material.lighting = lighting;
    material.raster = raster;
    return J3dLitAlphaTintResult::Success;
}

} // namespace sb::native_render
