#include <sunbright/native_render/j3d_unlit_material.h>

namespace sb::native_render {
namespace {

constexpr std::uint8_t kColor0Alpha0 = 4;
constexpr std::array<std::uint8_t, 8> kRasterColorPassThrough{0xC0, 0x40, 0xAF, 0xF0,
                                                              0xC1, 0x08, 0xBF, 0x80};
constexpr std::array<std::uint8_t, 8> kTextureTimesRaster{0xC0, 0x08, 0xF8, 0xAF,
                                                          0xC1, 0x08, 0xF2, 0xF0};

} // namespace

const char* j3d_unlit_material_result_name(J3dUnlitMaterialResult result) noexcept {
    switch (result) {
    case J3dUnlitMaterialResult::Success:
        return "success";
    case J3dUnlitMaterialResult::UnsupportedColorBlock:
        return "unsupported colour block";
    case J3dUnlitMaterialResult::Lighting:
        return "lighting";
    case J3dUnlitMaterialResult::MissingColorChannel:
        return "missing color channel";
    case J3dUnlitMaterialResult::TextureBinding:
        return "texture binding";
    case J3dUnlitMaterialResult::UnsupportedTevBlock:
        return "unsupported colour-stage block";
    case J3dUnlitMaterialResult::MultipleTevStages:
        return "multiple active colour stages";
    case J3dUnlitMaterialResult::UnsupportedColorProgram:
        return "unsupported colour program";
    case J3dUnlitMaterialResult::MissingVertexColor:
        return "missing vertex colour";
    }
    return "unknown";
}

const char* j3d_unlit_textured_result_name(J3dUnlitTexturedResult result) noexcept {
    switch (result) {
    case J3dUnlitTexturedResult::Success:
        return "success";
    case J3dUnlitTexturedResult::UnsupportedColorBlock:
        return "unsupported colour block";
    case J3dUnlitTexturedResult::Lighting:
        return "lighting";
    case J3dUnlitTexturedResult::MissingColorChannel:
        return "missing color channel";
    case J3dUnlitTexturedResult::UnsupportedTevBlock:
        return "unsupported colour-stage block";
    case J3dUnlitTexturedResult::MultipleTevStages:
        return "multiple active colour stages";
    case J3dUnlitTexturedResult::MissingTextureCoordinate:
        return "missing texture coordinate";
    case J3dUnlitTexturedResult::UnsupportedTextureBinding:
        return "unsupported texture binding";
    case J3dUnlitTexturedResult::UnsupportedColorProgram:
        return "unsupported colour program";
    case J3dUnlitTexturedResult::MissingVertexColor:
        return "missing vertex colour";
    }
    return "unknown";
}

J3dUnlitMaterialFeatures inspect_j3d_unlit_material(const J3dUnlitMaterialState& state) noexcept {
    const bool vertexColor = (state.colorChannelControl & 0x0001U) != 0;
    return {
        .supportedColorBlock = state.supportedColorBlock,
        .lightingEnabled = state.lightingEnabled,
        .hasColorChannel = state.colorChannelCount != 0,
        .textureBound = state.textureNumber0 != 0xFFFFU || state.textureCoordinate0 != 0xFFU ||
                        state.textureMap0 != 0xFFU,
        .supportedTevBlock = state.supportedTevBlock,
        .singleTevStage = state.tevStageCount == 1,
        .rasterColorPassThrough =
            state.colorChannel0 == kColor0Alpha0 && state.tevStage0 == kRasterColorPassThrough,
        .requiredVertexColorPresent = !vertexColor || state.hasVertexColor,
    };
}

J3dUnlitMaterialResult classify_j3d_unlit_material(const J3dUnlitMaterialState& state,
                                                   UnlitColorMaterial& material) noexcept {
    const J3dUnlitMaterialFeatures features = inspect_j3d_unlit_material(state);
    if (!features.supportedColorBlock)
        return J3dUnlitMaterialResult::UnsupportedColorBlock;
    if (features.lightingEnabled)
        return J3dUnlitMaterialResult::Lighting;
    if (!features.hasColorChannel)
        return J3dUnlitMaterialResult::MissingColorChannel;
    if (!features.supportedTevBlock)
        return J3dUnlitMaterialResult::UnsupportedTevBlock;
    if (!features.singleTevStage)
        return J3dUnlitMaterialResult::MultipleTevStages;
    if (features.textureBound)
        return J3dUnlitMaterialResult::TextureBinding;
    if (!features.rasterColorPassThrough)
        return J3dUnlitMaterialResult::UnsupportedColorProgram;

    const bool vertexColor = (state.colorChannelControl & 0x0001U) != 0;
    if (!features.requiredVertexColorPresent)
        return J3dUnlitMaterialResult::MissingVertexColor;
    material.usesVertexColor = vertexColor;
    material.baseColor =
        vertexColor ? Color{1.0F, 1.0F, 1.0F, 1.0F} : color_from_rgba8(state.materialColorRgba8);
    return J3dUnlitMaterialResult::Success;
}

J3dUnlitTexturedResult
classify_j3d_unlit_textured_material(const J3dUnlitMaterialState& state,
                                     const PictureTexture& texture,
                                     UnlitTexturedMaterial& material) noexcept {
    if (!state.supportedColorBlock)
        return J3dUnlitTexturedResult::UnsupportedColorBlock;
    if (state.lightingEnabled)
        return J3dUnlitTexturedResult::Lighting;
    if (state.colorChannelCount == 0)
        return J3dUnlitTexturedResult::MissingColorChannel;
    if (!state.supportedTevBlock)
        return J3dUnlitTexturedResult::UnsupportedTevBlock;
    if (state.tevStageCount != 1)
        return J3dUnlitTexturedResult::MultipleTevStages;
    if (state.textureCoordinateCount == 0)
        return J3dUnlitTexturedResult::MissingTextureCoordinate;
    if (state.textureNumber0 == 0xFFFFU || state.textureCoordinate0 != 0 ||
        state.textureMap0 != 0 || state.colorChannel0 != kColor0Alpha0 || texture.resource == 0 ||
        texture.width == 0 || texture.height == 0) {
        return J3dUnlitTexturedResult::UnsupportedTextureBinding;
    }
    if (state.tevStage0 != kTextureTimesRaster)
        return J3dUnlitTexturedResult::UnsupportedColorProgram;
    const bool vertexColor = (state.colorChannelControl & 0x0001U) != 0;
    if (vertexColor && !state.hasVertexColor)
        return J3dUnlitTexturedResult::MissingVertexColor;
    material.texture = texture;
    material.usesVertexColor = vertexColor;
    return J3dUnlitTexturedResult::Success;
}

} // namespace sb::native_render
