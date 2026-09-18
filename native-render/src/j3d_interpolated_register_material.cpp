#include <sunbright/native_render/j3d_interpolated_register_material.h>

#include <sunbright/native_render/j3d_unlit_material.h>

#include <array>

namespace sb::native_render {
namespace {

// The console's constant-selection enumeration, under decomp/sms/include/dolphin/gx/, names each
// authored constant's alpha broadcast to colour at the top of its colour selections. Which
// constant a stage means is its distance from the first of those.
constexpr std::uint8_t kFirstConstantAlphaAsColorSelection = 0x1C;
constexpr std::size_t kConstantColorCount = 4;

// prev = clamp(konst.rgb + lerp(c0.rgb, c1.rgb, tex.rgb)), alpha prev = clamp(tex.a * a0).
constexpr std::array<std::uint8_t, 8> kInterpolateRegisters{0xC0, 0x08, 0x24, 0x8E,
                                                            0xC1, 0x08, 0xF0, 0xF0};
// prev = clamp(((prev.rgb + tex.rgb * konst.rgb) + 0.5) / 2),
// alpha prev = clamp(prev.a + konst.a * prev.a).
constexpr std::array<std::uint8_t, 8> kAverageWeightedDetail{0xC2, 0x39, 0xF8, 0xE0,
                                                             0xC3, 0x08, 0xF8, 0x00};

} // namespace

const char* j3d_interpolated_register_result_name(J3dInterpolatedRegisterResult result) noexcept {
    switch (result) {
    case J3dInterpolatedRegisterResult::Success:
        return "success";
    case J3dInterpolatedRegisterResult::UnsupportedColorBlock:
        return "unsupported colour block";
    case J3dInterpolatedRegisterResult::UnsupportedTevBlock:
        return "unsupported colour-stage block";
    case J3dInterpolatedRegisterResult::UnsupportedStageCount:
        return "unsupported colour-stage count";
    case J3dInterpolatedRegisterResult::MissingTextureCoordinate:
        return "missing texture coordinate";
    case J3dInterpolatedRegisterResult::UnsupportedTextureBinding:
        return "unsupported texture binding";
    case J3dInterpolatedRegisterResult::UnsupportedColorProgram:
        return "unsupported interpolated-register colour program";
    case J3dInterpolatedRegisterResult::MissingRegisterColor:
        return "missing colour register";
    case J3dInterpolatedRegisterResult::UnsupportedRasterPolicy:
        return "unsupported raster policy";
    }
    return "unknown";
}

J3dInterpolatedRegisterResult classify_j3d_interpolated_register_material(
    const J3dMaterialState& state, const PictureTexture& blend, const PictureTexture& detail,
    InterpolatedRegisterMaterial& material) noexcept {
    if (!state.supportedColorBlock) {
        return J3dInterpolatedRegisterResult::UnsupportedColorBlock;
    }
    if (!state.supportedTevBlock) {
        return J3dInterpolatedRegisterResult::UnsupportedTevBlock;
    }
    if (state.tevStageCount != 2) {
        return J3dInterpolatedRegisterResult::UnsupportedStageCount;
    }
    const J3dTevStageState& first = state.tevStages[0];
    const J3dTevStageState& second = state.tevStages[1];
    const std::uint8_t offsetConstant =
        static_cast<std::uint8_t>(first.konstColorSelection - kFirstConstantAlphaAsColorSelection);
    if (first.program != kInterpolateRegisters || second.program != kAverageWeightedDetail ||
        first.konstColorSelection < kFirstConstantAlphaAsColorSelection ||
        offsetConstant >= kConstantColorCount ||
        !selects(second.konstColorSelection, J3dKonstFraction::FiveEighths) ||
        !selects(second.konstAlphaSelection, J3dKonstFraction::ThreeEighths)) {
        return J3dInterpolatedRegisterResult::UnsupportedColorProgram;
    }
    if (state.textureCoordinateCount < 2) {
        return J3dInterpolatedRegisterResult::MissingTextureCoordinate;
    }
    const auto boundTexture = [](const PictureTexture& texture) {
        return texture.resource != 0 && texture.width != 0 && texture.height != 0;
    };
    if (first.textureCoordinate != 0 || second.textureCoordinate != 1 ||
        j3d_texture_number_for_map(state, first.textureMap) == 0xFFFFU ||
        j3d_texture_number_for_map(state, second.textureMap) == 0xFFFFU || !boundTexture(blend) ||
        !boundTexture(detail)) {
        return J3dInterpolatedRegisterResult::UnsupportedTextureBinding;
    }
    if (!state.hasTevColors) {
        return J3dInterpolatedRegisterResult::MissingRegisterColor;
    }
    ModelRasterPolicy raster{};
    if (classify_j3d_raster_policy(state, raster) != J3dRasterPolicyResult::Success) {
        return J3dInterpolatedRegisterResult::UnsupportedRasterPolicy;
    }

    material.blendTexture = blend;
    material.detailTexture = detail;
    material.lowerColor = color_from_s10(state.tevColorsS10[0]);
    material.upperColor = color_from_s10_rgb(state.tevColorsS10[1]);
    material.colorOffset = color_from_rgba8(state.konstColorRgba8[offsetConstant]).a;
    material.detailWeight = value_of(J3dKonstFraction::FiveEighths);
    // The second stage's alpha adds a fraction of what it already has, so opacity comes out scaled
    // by one plus that fraction rather than replaced by it.
    material.alphaGain = 1.0F + value_of(J3dKonstFraction::ThreeEighths);
    material.raster = raster;
    return J3dInterpolatedRegisterResult::Success;
}

} // namespace sb::native_render
