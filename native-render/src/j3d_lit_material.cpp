#include <sunbright/native_render/j3d_lit_material.h>

#include <sunbright/native_render/j3d_unlit_material.h>

#include <array>

namespace sb::native_render {
namespace {

constexpr std::uint8_t kColor0Alpha0 = 4;
constexpr std::uint16_t kDiffuseMaterialColor = 0x070E;
constexpr std::uint16_t kPrimaryLightDiffuseColor = 0x0706;
constexpr std::uint16_t kDiffuseVertexColor = 0x070F;
constexpr std::uint16_t kHalfDiffuseVertexColor = 0x0707;
constexpr std::uint16_t kUnlitMaterialAlpha = 0x0700;
constexpr std::uint16_t kUnlitVertexAlpha = 0x0701;
constexpr std::array<std::uint8_t, 8> kTextureTimesRaster{0xC0, 0x08, 0xF8, 0xAF,
                                                          0xC1, 0x08, 0xF2, 0xF0};
constexpr std::array<std::uint8_t, 8> kTextureOverShadow{0xC0, 0x08, 0xE8, 0xAF,
                                                         0xC1, 0x08, 0xF2, 0xF0};
constexpr std::array<std::uint8_t, 8> kRasterPassThrough{0xC0, 0x08, 0xAF, 0xFF,
                                                         0xC1, 0x08, 0xBF, 0xF0};
constexpr std::array<std::uint8_t, 8> kHalfDiffuseStage{0xC0, 0x08, 0xCA, 0xEF,
                                                        0xC1, 0x08, 0xFF, 0xD0};
constexpr std::array<std::uint8_t, 8> kHalfDiffuseTextureStage{0xC2, 0x08, 0xF0, 0x8F,
                                                               0xC3, 0x08, 0xF0, 0x70};

struct StandardDiffuseChannels {
    bool supported = false;
    bool usesPrimaryLight = false;
    bool usesVertexRgb = false;
    bool usesVertexAlpha = false;
};

StandardDiffuseChannels standard_diffuse_channels(const J3dMaterialState& state) noexcept {
    if (state.colorChannelControl == kPrimaryLightDiffuseColor &&
        state.alphaChannelControl == kUnlitMaterialAlpha) {
        return {.supported = true, .usesPrimaryLight = true};
    }
    if (state.colorChannelControl == kDiffuseMaterialColor &&
        state.alphaChannelControl == kUnlitMaterialAlpha) {
        return {.supported = true};
    }
    if (state.colorChannelControl == kDiffuseVertexColor &&
        state.alphaChannelControl == kUnlitVertexAlpha) {
        return {.supported = true, .usesVertexRgb = true, .usesVertexAlpha = true};
    }
    if (state.colorChannelControl == kDiffuseVertexColor &&
        state.alphaChannelControl == kUnlitMaterialAlpha) {
        return {.supported = true, .usesVertexRgb = true};
    }
    return {};
}

bool valid_lighting(const ModelLightingContext& lighting) noexcept {
    return lighting.pointLightCount != 0 && lighting.pointLightCount <= lighting.pointLights.size();
}

} // namespace

const char* j3d_lit_color_result_name(J3dLitColorResult result) noexcept {
    switch (result) {
    case J3dLitColorResult::Success:
        return "success";
    case J3dLitColorResult::UnsupportedColorBlock:
        return "unsupported colour block";
    case J3dLitColorResult::UnsupportedColorChannels:
        return "unsupported colour channels";
    case J3dLitColorResult::UnsupportedTevBlock:
        return "unsupported colour-stage block";
    case J3dLitColorResult::UnsupportedStageCount:
        return "unsupported colour-stage count";
    case J3dLitColorResult::TextureBinding:
        return "texture binding";
    case J3dLitColorResult::UnsupportedColorProgram:
        return "unsupported colour program";
    case J3dLitColorResult::MissingNormal:
        return "missing normal";
    case J3dLitColorResult::MissingVertexColor:
        return "missing vertex colour";
    case J3dLitColorResult::MissingLightingContext:
        return "missing lighting context";
    case J3dLitColorResult::UnsupportedRasterPolicy:
        return "unsupported raster policy";
    }
    return "unknown";
}

J3dLitColorResult classify_j3d_lit_color_material(const J3dMaterialState& state,
                                                  const ModelLightingContext& lighting,
                                                  LitColorMaterial& material) noexcept {
    if (!state.supportedColorBlock)
        return J3dLitColorResult::UnsupportedColorBlock;
    const StandardDiffuseChannels channels = standard_diffuse_channels(state);
    if (!state.lightingEnabled || state.colorChannelCount == 0 || !channels.supported)
        return J3dLitColorResult::UnsupportedColorChannels;
    if (!state.supportedTevBlock)
        return J3dLitColorResult::UnsupportedTevBlock;
    if (state.tevStageCount != 1)
        return J3dLitColorResult::UnsupportedStageCount;
    if (state.textureBindings[0].textureNumber != 0xFFFFU ||
        state.tevStages[0].textureCoordinate != 0xFFU || state.tevStages[0].textureMap != 0xFFU ||
        state.tevStages[0].colorChannel != kColor0Alpha0) {
        return J3dLitColorResult::TextureBinding;
    }
    if (state.tevStages[0].program != kRasterPassThrough)
        return J3dLitColorResult::UnsupportedColorProgram;
    if (!state.hasNormal)
        return J3dLitColorResult::MissingNormal;
    if ((channels.usesVertexRgb || channels.usesVertexAlpha) && !state.hasVertexColor)
        return J3dLitColorResult::MissingVertexColor;
    if (!valid_lighting(lighting))
        return J3dLitColorResult::MissingLightingContext;
    ModelRasterPolicy raster{};
    if (classify_j3d_raster_policy(state, raster) != J3dRasterPolicyResult::Success)
        return J3dLitColorResult::UnsupportedRasterPolicy;

    material.baseColor = color_from_rgba8(state.materialColorRgba8);
    material.ambientColor = state.usesMaterialAmbient ? color_from_rgba8(state.ambientColorRgba8)
                                                      : lighting.ambientColor;
    material.lighting = lighting;
    if (channels.usesPrimaryLight)
        material.lighting.pointLightCount = 1;
    material.usesVertexRgb = channels.usesVertexRgb;
    material.usesVertexAlpha = channels.usesVertexAlpha;
    material.raster = raster;
    return J3dLitColorResult::Success;
}

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
    const StandardDiffuseChannels channels = standard_diffuse_channels(state);
    const bool halfDiffuse = state.colorChannelControl == kHalfDiffuseVertexColor &&
                             state.alphaChannelControl == kUnlitMaterialAlpha;
    if (!state.lightingEnabled || state.colorChannelCount == 0 ||
        (!channels.supported && !halfDiffuse)) {
        return J3dLitTexturedResult::UnsupportedColorChannels;
    }
    if (!state.supportedTevBlock)
        return J3dLitTexturedResult::UnsupportedTevBlock;
    const std::uint8_t expectedStageCount = halfDiffuse ? 2 : 1;
    if (state.tevStageCount != expectedStageCount)
        return J3dLitTexturedResult::UnsupportedStageCount;
    if (state.textureCoordinateCount == 0)
        return J3dLitTexturedResult::MissingTextureCoordinate;
    const bool standardBinding = state.tevStages[0].textureCoordinate == 0 &&
                                 state.tevStages[0].textureMap == 0 &&
                                 state.tevStages[0].colorChannel == kColor0Alpha0;
    const bool halfDiffuseBinding =
        state.tevStages[0].textureCoordinate == 0xFF && state.tevStages[0].textureMap == 0xFF &&
        state.tevStages[0].colorChannel == kColor0Alpha0 &&
        state.textureBindings[1].textureNumber == 0xFFFFU &&
        state.tevStages[1].textureCoordinate == 0 && state.tevStages[1].textureMap == 0 &&
        state.tevStages[1].colorChannel == 0xFF;
    if (state.textureBindings[0].textureNumber == 0xFFFFU ||
        (halfDiffuse ? !halfDiffuseBinding : !standardBinding) || texture.resource == 0 ||
        texture.width == 0 || texture.height == 0) {
        return J3dLitTexturedResult::UnsupportedTextureBinding;
    }
    const bool textureTimesRaster = state.tevStages[0].program == kTextureTimesRaster;
    const bool textureOverShadow = state.tevStages[0].program == kTextureOverShadow &&
                                   state.tevStages[0].konstColorSelection == 0x07 &&
                                   channels.usesVertexRgb && channels.usesVertexAlpha;
    const bool standardProgram = textureTimesRaster || textureOverShadow;
    const bool halfDiffuseProgram = state.tevStages[0].program == kHalfDiffuseStage &&
                                    state.tevStages[1].program == kHalfDiffuseTextureStage &&
                                    state.tevStages[0].konstColorSelection == 0x04;
    if (halfDiffuse ? !halfDiffuseProgram : !standardProgram)
        return J3dLitTexturedResult::UnsupportedColorProgram;
    if (!state.hasNormal)
        return J3dLitTexturedResult::MissingNormal;
    if ((channels.usesVertexRgb || halfDiffuse) && !state.hasVertexColor)
        return J3dLitTexturedResult::MissingVertexColor;
    if (!valid_lighting(lighting))
        return J3dLitTexturedResult::MissingLightingContext;
    ModelRasterPolicy raster{};
    if (classify_j3d_raster_policy(state, raster) != J3dRasterPolicyResult::Success)
        return J3dLitTexturedResult::UnsupportedRasterPolicy;

    material.texture = texture;
    material.baseColor = color_from_rgba8(state.materialColorRgba8);
    material.ambientColor = state.usesMaterialAmbient ? color_from_rgba8(state.ambientColorRgba8)
                                                      : lighting.ambientColor;
    material.shadowColor = textureOverShadow ? Color{0.125F, 0.125F, 0.125F, 0.0F} : Color{};
    material.lighting = lighting;
    if (channels.usesPrimaryLight)
        material.lighting.pointLightCount = 1;
    material.litColorWeight = halfDiffuse ? 0.5F : 1.0F;
    material.usesVertexRgb = channels.usesVertexRgb || halfDiffuse;
    material.usesVertexAlpha = channels.usesVertexAlpha;
    material.raster = raster;
    return J3dLitTexturedResult::Success;
}

} // namespace sb::native_render
