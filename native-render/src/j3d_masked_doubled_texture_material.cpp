#include <sunbright/native_render/j3d_masked_doubled_texture_material.h>

#include <sunbright/native_render/j3d_unlit_material.h>

#include <array>

namespace sb::native_render {
namespace {

constexpr std::uint16_t kMaterialColorChannel = 0x0700;
constexpr std::uint8_t kColor0Alpha0 = 4;

// prev = clamp(tex.rgb * ras.rgb), alpha prev = clamp(tex.a * ras.a).
constexpr std::array<std::uint8_t, 8> kTextureTimesRaster{0xC0, 0x08, 0xF8, 0xAF,
                                                          0xC1, 0x08, 0xF2, 0xF0};
// prev = clamp((tex.rgb * ras.rgb) * 2), alpha prev = tex.a * prev.a.
constexpr std::array<std::uint8_t, 8> kTextureTimesRasterDoubled{0xC2, 0x18, 0xF8, 0xAF,
                                                                 0xC3, 0x00, 0xF0, 0x70};

} // namespace

const char* j3d_masked_doubled_texture_result_name(J3dMaskedDoubledTextureResult result) noexcept {
    switch (result) {
    case J3dMaskedDoubledTextureResult::Success:
        return "success";
    case J3dMaskedDoubledTextureResult::UnsupportedColorBlock:
        return "unsupported colour block";
    case J3dMaskedDoubledTextureResult::Lighting:
        return "lighting";
    case J3dMaskedDoubledTextureResult::UnsupportedColorChannels:
        return "unsupported masked-doubled colour channels";
    case J3dMaskedDoubledTextureResult::UnsupportedTevBlock:
        return "unsupported colour-stage block";
    case J3dMaskedDoubledTextureResult::UnsupportedStageCount:
        return "unsupported colour-stage count";
    case J3dMaskedDoubledTextureResult::MissingTextureCoordinate:
        return "missing texture coordinate";
    case J3dMaskedDoubledTextureResult::UnsupportedTextureBinding:
        return "unsupported texture binding";
    case J3dMaskedDoubledTextureResult::UnsupportedColorProgram:
        return "unsupported masked-doubled colour program";
    case J3dMaskedDoubledTextureResult::UnsupportedRasterPolicy:
        return "unsupported raster policy";
    }
    return "unknown";
}

J3dMaskedDoubledTextureResult classify_j3d_masked_doubled_texture_material(
    const J3dMaterialState& state, const PictureTexture& opacity, const PictureTexture& color,
    MaskedDoubledTextureMaterial& material) noexcept {
    if (!state.supportedColorBlock) {
        return J3dMaskedDoubledTextureResult::UnsupportedColorBlock;
    }
    if (!state.supportedTevBlock) {
        return J3dMaskedDoubledTextureResult::UnsupportedTevBlock;
    }
    if (state.tevStageCount != 2) {
        return J3dMaskedDoubledTextureResult::UnsupportedStageCount;
    }
    const J3dTevStageState& first = state.tevStages[0];
    const J3dTevStageState& second = state.tevStages[1];
    if (first.program != kTextureTimesRaster || second.program != kTextureTimesRasterDoubled) {
        return J3dMaskedDoubledTextureResult::UnsupportedColorProgram;
    }
    // Both stages read the rasterised channel, so lighting it would change what is drawn.
    if (state.lightingEnabled) {
        return J3dMaskedDoubledTextureResult::Lighting;
    }
    if (state.colorChannelCount != 1 || state.colorChannelControl != kMaterialColorChannel ||
        state.alphaChannelControl != kMaterialColorChannel || first.colorChannel != kColor0Alpha0 ||
        second.colorChannel != kColor0Alpha0) {
        return J3dMaskedDoubledTextureResult::UnsupportedColorChannels;
    }
    if (state.textureCoordinateCount < 2) {
        return J3dMaskedDoubledTextureResult::MissingTextureCoordinate;
    }
    const auto boundTexture = [](const PictureTexture& texture) {
        return texture.resource != 0 && texture.width != 0 && texture.height != 0;
    };
    if (first.textureCoordinate != 0 || second.textureCoordinate != 1 ||
        j3d_texture_number_for_map(state, first.textureMap) == 0xFFFFU ||
        j3d_texture_number_for_map(state, second.textureMap) == 0xFFFFU || !boundTexture(opacity) ||
        !boundTexture(color)) {
        return J3dMaskedDoubledTextureResult::UnsupportedTextureBinding;
    }
    ModelRasterPolicy raster{};
    if (classify_j3d_raster_policy(state, raster) != J3dRasterPolicyResult::Success) {
        return J3dMaskedDoubledTextureResult::UnsupportedRasterPolicy;
    }

    material.opacityTexture = opacity;
    material.colorTexture = color;
    material.tint = color_from_rgba8(state.materialColorRgba8);
    material.raster = raster;
    return J3dMaskedDoubledTextureResult::Success;
}

} // namespace sb::native_render
