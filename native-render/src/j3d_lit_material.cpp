#include <sunbright/native_render/j3d_lit_material.h>

#include <sunbright/native_render/j3d_unlit_material.h>

#include <array>

namespace sb::native_render {
namespace {

constexpr std::uint8_t kColor0Alpha0 = 4;
constexpr std::uint16_t kDiffuseMaterialColor = 0x070E;
constexpr std::uint16_t kDiffuseVertexColor = 0x070F;
constexpr std::uint16_t kHalfDiffuseVertexColor = 0x0707;
constexpr std::uint16_t kUnlitMaterialAlpha = 0x0700;
constexpr std::uint16_t kUnlitVertexAlpha = 0x0701;
constexpr std::array<std::uint8_t, 8> kTextureTimesRaster{0xC0, 0x08, 0xF8, 0xAF,
                                                          0xC1, 0x08, 0xF2, 0xF0};
constexpr std::array<std::uint8_t, 8> kHalfDiffuseStage{0xC0, 0x08, 0xCA, 0xEF,
                                                        0xC1, 0x08, 0xFF, 0xD0};
constexpr std::array<std::uint8_t, 8> kHalfDiffuseTextureStage{0xC2, 0x08, 0xF0, 0x8F,
                                                               0xC3, 0x08, 0xF0, 0x70};

} // namespace

const char* j3d_lit_textured_result_name(J3dLitTexturedResult result) noexcept {
    switch (result) {
    case J3dLitTexturedResult::Success:
        return "success";
    case J3dLitTexturedResult::UnsupportedColorBlock:
        return "unsupported colour block";
    case J3dLitTexturedResult::UnsupportedColorChannels:
        return "unsupported colour channels";
    case J3dLitTexturedResult::UnsupportedTevBlock:
        return "unsupported colour-stage block";
    case J3dLitTexturedResult::UnsupportedStageCount:
        return "unsupported colour-stage count";
    case J3dLitTexturedResult::MissingTextureCoordinate:
        return "missing texture coordinate";
    case J3dLitTexturedResult::UnsupportedTextureBinding:
        return "unsupported texture binding";
    case J3dLitTexturedResult::UnsupportedColorProgram:
        return "unsupported colour program";
    case J3dLitTexturedResult::MissingNormal:
        return "missing normal";
    case J3dLitTexturedResult::MissingVertexColor:
        return "missing vertex colour";
    case J3dLitTexturedResult::MissingLightingContext:
        return "missing lighting context";
    case J3dLitTexturedResult::UnsupportedRasterPolicy:
        return "unsupported raster policy";
    }
    return "unknown";
}

J3dLitTexturedResult classify_j3d_lit_textured_material(const J3dMaterialState& state,
                                                        const PictureTexture& texture,
                                                        const ModelLightingContext& lighting,
                                                        LitTexturedMaterial& material) noexcept {
    if (!state.supportedColorBlock)
        return J3dLitTexturedResult::UnsupportedColorBlock;
    const bool materialColor = state.colorChannelControl == kDiffuseMaterialColor &&
                               state.alphaChannelControl == kUnlitMaterialAlpha;
    const bool vertexColor = state.colorChannelControl == kDiffuseVertexColor &&
                             state.alphaChannelControl == kUnlitVertexAlpha;
    const bool halfDiffuse = state.colorChannelControl == kHalfDiffuseVertexColor &&
                             state.alphaChannelControl == kUnlitMaterialAlpha;
    if (!state.lightingEnabled || state.colorChannelCount == 0 ||
        (!materialColor && !vertexColor && !halfDiffuse)) {
        return J3dLitTexturedResult::UnsupportedColorChannels;
    }
    if (!state.supportedTevBlock)
        return J3dLitTexturedResult::UnsupportedTevBlock;
    const std::uint8_t expectedStageCount = halfDiffuse ? 2 : 1;
    if (state.tevStageCount != expectedStageCount)
        return J3dLitTexturedResult::UnsupportedStageCount;
    if (state.textureCoordinateCount == 0)
        return J3dLitTexturedResult::MissingTextureCoordinate;
    const bool standardBinding = state.textureCoordinate0 == 0 && state.textureMap0 == 0 &&
                                 state.colorChannel0 == kColor0Alpha0;
    const bool halfDiffuseBinding =
        state.textureCoordinate0 == 0xFF && state.textureMap0 == 0xFF &&
        state.colorChannel0 == kColor0Alpha0 && state.textureNumber1 == 0xFFFFU &&
        state.textureCoordinate1 == 0 && state.textureMap1 == 0 && state.colorChannel1 == 0xFF;
    if (state.textureNumber0 == 0xFFFFU || (halfDiffuse ? !halfDiffuseBinding : !standardBinding) ||
        texture.resource == 0 || texture.width == 0 || texture.height == 0) {
        return J3dLitTexturedResult::UnsupportedTextureBinding;
    }
    const bool standardProgram = state.tevStage0 == kTextureTimesRaster;
    const bool halfDiffuseProgram = state.tevStage0 == kHalfDiffuseStage &&
                                    state.tevStage1 == kHalfDiffuseTextureStage &&
                                    state.konstColorSelection0 == 0x04;
    if (halfDiffuse ? !halfDiffuseProgram : !standardProgram)
        return J3dLitTexturedResult::UnsupportedColorProgram;
    if (!state.hasNormal)
        return J3dLitTexturedResult::MissingNormal;
    if ((vertexColor || halfDiffuse) && !state.hasVertexColor)
        return J3dLitTexturedResult::MissingVertexColor;
    if (lighting.pointLightCount == 0 || lighting.pointLightCount > lighting.pointLights.size())
        return J3dLitTexturedResult::MissingLightingContext;
    ModelRasterPolicy raster{};
    if (classify_j3d_raster_policy(state, raster) != J3dRasterPolicyResult::Success)
        return J3dLitTexturedResult::UnsupportedRasterPolicy;

    material.texture = texture;
    material.baseColor = color_from_rgba8(state.materialColorRgba8);
    material.ambientColor = state.usesMaterialAmbient ? color_from_rgba8(state.ambientColorRgba8)
                                                      : lighting.ambientColor;
    material.lighting = lighting;
    material.litColorWeight = halfDiffuse ? 0.5F : 1.0F;
    material.usesVertexRgb = vertexColor || halfDiffuse;
    material.usesVertexAlpha = vertexColor;
    material.raster = raster;
    return J3dLitTexturedResult::Success;
}

} // namespace sb::native_render
