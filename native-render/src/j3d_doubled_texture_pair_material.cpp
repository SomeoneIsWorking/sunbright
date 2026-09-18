#include <sunbright/native_render/j3d_doubled_texture_pair_material.h>

#include <sunbright/native_render/j3d_unlit_material.h>

#include <algorithm>
#include <array>

namespace sb::native_render {
namespace {

constexpr std::uint8_t kColor0Alpha0 = 4;
constexpr std::uint8_t kVertexSource = 0x01;

// The console's constant-selection enumeration, under decomp/sms/include/dolphin/gx/, names the
// four authored colour constants at the top of its colour selections and their alphas at the top of
// its alpha selections. The register a selection names is its distance from the first of them.
constexpr std::uint8_t kFirstConstantColorSelection = 0x0C;
constexpr std::uint8_t kFirstConstantAlphaSelection = 0x1C;
constexpr std::size_t kConstantColorCount = 4;

// prev = tex.rgb * ras.rgb, alpha prev = tex.a * ras.a.
constexpr std::array<std::uint8_t, 8> kTextureTimesRaster{0xC0, 0x00, 0xF8, 0xAF,
                                                          0xC1, 0x00, 0xF2, 0xF0};
// prev = clamp((tex.rgb * prev.rgb) * 2), alpha prev = clamp((tex.a * prev.a) * 2).
constexpr std::array<std::uint8_t, 8> kTextureTimesPreviousDoubled{0xC2, 0x18, 0xF8, 0x0F,
                                                                   0xC3, 0x18, 0xF0, 0x70};
// prev = clamp(konst.rgb * tex.rgb), alpha prev = clamp(konst.a * a0).
constexpr std::array<std::uint8_t, 8> kConstantTimesTexture{0xC0, 0x08, 0xFE, 0x8F,
                                                            0xC1, 0x08, 0xF8, 0xF0};
// prev = (prev.rgb * tex.rgb) * 2, alpha prev = prev.a.
constexpr std::array<std::uint8_t, 8> kPreviousTimesTextureDoubled{0xC2, 0x10, 0xF0, 0x8F,
                                                                   0xC3, 0x00, 0xFF, 0x80};

[[nodiscard]] bool names_constant_register(std::uint8_t colorSelection,
                                           std::uint8_t alphaSelection) noexcept {
    const std::uint8_t colorIndex = colorSelection - kFirstConstantColorSelection;
    const std::uint8_t alphaIndex = alphaSelection - kFirstConstantAlphaSelection;
    return colorSelection >= kFirstConstantColorSelection && colorIndex < kConstantColorCount &&
           alphaSelection >= kFirstConstantAlphaSelection && alphaIndex < kConstantColorCount;
}

} // namespace

const char* j3d_doubled_texture_pair_result_name(J3dDoubledTexturePairResult result) noexcept {
    switch (result) {
    case J3dDoubledTexturePairResult::Success:
        return "success";
    case J3dDoubledTexturePairResult::UnsupportedColorBlock:
        return "unsupported colour block";
    case J3dDoubledTexturePairResult::UnsupportedColorChannels:
        return "unsupported texture-pair colour channels";
    case J3dDoubledTexturePairResult::UnsupportedTevBlock:
        return "unsupported colour-stage block";
    case J3dDoubledTexturePairResult::UnsupportedStageCount:
        return "unsupported colour-stage count";
    case J3dDoubledTexturePairResult::MissingTextureCoordinate:
        return "missing texture coordinate";
    case J3dDoubledTexturePairResult::UnsupportedTextureBinding:
        return "unsupported texture binding";
    case J3dDoubledTexturePairResult::UnsupportedColorProgram:
        return "unsupported texture-pair colour program";
    case J3dDoubledTexturePairResult::MissingRegisterColor:
        return "missing colour register";
    case J3dDoubledTexturePairResult::MissingVertexColor:
        return "missing vertex colour";
    case J3dDoubledTexturePairResult::UnsupportedRasterPolicy:
        return "unsupported raster policy";
    }
    return "unknown";
}

J3dDoubledTexturePairResult
classify_j3d_doubled_texture_pair_material(const J3dMaterialState& state,
                                           const PictureTexture& base, const PictureTexture& detail,
                                           DoubledTexturePairMaterial& material) noexcept {
    if (!state.supportedColorBlock) {
        return J3dDoubledTexturePairResult::UnsupportedColorBlock;
    }
    if (!state.supportedTevBlock) {
        return J3dDoubledTexturePairResult::UnsupportedTevBlock;
    }
    if (state.tevStageCount != 2) {
        return J3dDoubledTexturePairResult::UnsupportedStageCount;
    }
    const J3dTevStageState& first = state.tevStages[0];
    const J3dTevStageState& second = state.tevStages[1];

    const bool rasterTint =
        first.program == kTextureTimesRaster && second.program == kTextureTimesPreviousDoubled;
    const bool constantTint =
        first.program == kConstantTimesTexture && second.program == kPreviousTimesTextureDoubled &&
        names_constant_register(first.konstColorSelection, first.konstAlphaSelection);
    if (!rasterTint && !constantTint) {
        return J3dDoubledTexturePairResult::UnsupportedColorProgram;
    }

    // The raster spelling reads the rasterised channel in its first stage, so that channel decides
    // what is drawn and lighting would change it. The constant spelling reads no raster input at
    // all: its channel output is unread, which is why a lit channel is admitted there and only
    // there.
    if (rasterTint) {
        if (state.lightingEnabled) {
            return J3dDoubledTexturePairResult::UnsupportedColorChannels;
        }
        if (state.colorChannelCount != 1 || first.colorChannel != kColor0Alpha0) {
            return J3dDoubledTexturePairResult::UnsupportedColorChannels;
        }
    }

    if (state.textureCoordinateCount < 2) {
        return J3dDoubledTexturePairResult::MissingTextureCoordinate;
    }
    const auto boundTexture = [](const PictureTexture& texture) {
        return texture.resource != 0 && texture.width != 0 && texture.height != 0;
    };
    if (first.textureCoordinate != 0 || second.textureCoordinate != 1 ||
        j3d_texture_number_for_map(state, first.textureMap) == 0xFFFFU ||
        j3d_texture_number_for_map(state, second.textureMap) == 0xFFFFU || !boundTexture(base) ||
        !boundTexture(detail)) {
        return J3dDoubledTexturePairResult::UnsupportedTextureBinding;
    }

    ModelRasterPolicy raster{};
    if (classify_j3d_raster_policy(state, raster) != J3dRasterPolicyResult::Success) {
        return J3dDoubledTexturePairResult::UnsupportedRasterPolicy;
    }

    Color tint{};
    bool tintRgbFromVertexColor = false;
    bool tintAlphaFromVertexColor = false;
    ModelPairAlphaMode alphaMode = ModelPairAlphaMode::DoubledTextureProduct;
    if (rasterTint) {
        tintRgbFromVertexColor = (state.colorChannelControl & kVertexSource) != 0;
        tintAlphaFromVertexColor = (state.alphaChannelControl & kVertexSource) != 0;
        if ((tintRgbFromVertexColor || tintAlphaFromVertexColor) && !state.hasVertexColor) {
            return J3dDoubledTexturePairResult::MissingVertexColor;
        }
        tint = color_from_rgba8(state.materialColorRgba8);
    } else {
        if (!state.hasTevColors) {
            return J3dDoubledTexturePairResult::MissingRegisterColor;
        }
        alphaMode = ModelPairAlphaMode::Constant;
        const Color constantColor = color_from_rgba8(
            state.konstColorRgba8[first.konstColorSelection - kFirstConstantColorSelection]);
        const Color constantAlpha = color_from_rgba8(
            state.konstColorRgba8[first.konstAlphaSelection - kFirstConstantAlphaSelection]);
        // The first stage's alpha multiplies the selected constant's alpha by a colour register's,
        // and the second carries the product through unchanged, so opacity is one authored number.
        tint = {constantColor.r, constantColor.g, constantColor.b,
                std::clamp(constantAlpha.a * color_from_s10(state.tevColorsS10[0]).a, 0.0F, 1.0F)};
    }

    material.baseTexture = base;
    material.detailTexture = detail;
    material.tint = tint;
    material.tintRgbFromVertexColor = tintRgbFromVertexColor;
    material.tintAlphaFromVertexColor = tintAlphaFromVertexColor;
    material.alphaMode = alphaMode;
    material.raster = raster;
    return J3dDoubledTexturePairResult::Success;
}

} // namespace sb::native_render
