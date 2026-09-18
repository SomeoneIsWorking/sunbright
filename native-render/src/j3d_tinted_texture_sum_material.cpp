#include <sunbright/native_render/j3d_tinted_texture_sum_material.h>

#include <sunbright/native_render/j3d_unlit_material.h>

#include <array>

namespace sb::native_render {
namespace {

// The console's constant-selection enumeration, under decomp/sms/include/dolphin/gx/, names its
// four authored colour constants at the top of its colour selections. Which one a stage means is
// its distance from the first of them, so a material that selects a different constant publishes a
// different tint rather than reading a fixed slot.
constexpr std::uint8_t kFirstConstantColorSelection = 0x0C;
constexpr std::size_t kConstantColorCount = 4;

// prev = clamp(konst.rgb * tex.rgb), alpha prev = clamp(a0 * tex.a).
constexpr std::array<std::uint8_t, 8> kConstantTimesTexture{0xC0, 0x08, 0xFE, 0x8F,
                                                            0xC1, 0x08, 0xE6, 0x70};
// prev = clamp(prev.rgb + konst.rgb * tex.rgb), alpha prev = clamp(prev.a * tex.a).
constexpr std::array<std::uint8_t, 8> kAddConstantTimesTexture{0xC2, 0x08, 0xFE, 0x80,
                                                               0xC3, 0x08, 0xE2, 0x70};

[[nodiscard]] bool names_constant_color(std::uint8_t selection) noexcept {
    return selection >= kFirstConstantColorSelection &&
           static_cast<std::uint8_t>(selection - kFirstConstantColorSelection) <
               kConstantColorCount;
}

[[nodiscard]] Color selected_constant(const J3dMaterialState& state,
                                      std::uint8_t selection) noexcept {
    return color_from_rgba8(state.konstColorRgba8[selection - kFirstConstantColorSelection]);
}

} // namespace

const char* j3d_tinted_texture_sum_result_name(J3dTintedTextureSumResult result) noexcept {
    switch (result) {
    case J3dTintedTextureSumResult::Success:
        return "success";
    case J3dTintedTextureSumResult::UnsupportedColorBlock:
        return "unsupported colour block";
    case J3dTintedTextureSumResult::UnsupportedTevBlock:
        return "unsupported colour-stage block";
    case J3dTintedTextureSumResult::UnsupportedStageCount:
        return "unsupported colour-stage count";
    case J3dTintedTextureSumResult::MissingTextureCoordinate:
        return "missing texture coordinate";
    case J3dTintedTextureSumResult::UnsupportedTextureBinding:
        return "unsupported texture binding";
    case J3dTintedTextureSumResult::UnsupportedColorProgram:
        return "unsupported texture-sum colour program";
    case J3dTintedTextureSumResult::MissingRegisterColor:
        return "missing colour register";
    case J3dTintedTextureSumResult::UnsupportedRasterPolicy:
        return "unsupported raster policy";
    }
    return "unknown";
}

J3dTintedTextureSumResult
classify_j3d_tinted_texture_sum_material(const J3dMaterialState& state, const PictureTexture& first,
                                         const PictureTexture& second,
                                         TintedTextureSumMaterial& material) noexcept {
    if (!state.supportedColorBlock) {
        return J3dTintedTextureSumResult::UnsupportedColorBlock;
    }
    if (!state.supportedTevBlock) {
        return J3dTintedTextureSumResult::UnsupportedTevBlock;
    }
    if (state.tevStageCount != 2) {
        return J3dTintedTextureSumResult::UnsupportedStageCount;
    }
    const J3dTevStageState& firstStage = state.tevStages[0];
    const J3dTevStageState& secondStage = state.tevStages[1];
    if (firstStage.program != kConstantTimesTexture ||
        secondStage.program != kAddConstantTimesTexture ||
        !names_constant_color(firstStage.konstColorSelection) ||
        !names_constant_color(secondStage.konstColorSelection)) {
        return J3dTintedTextureSumResult::UnsupportedColorProgram;
    }
    if (state.textureCoordinateCount < 2) {
        return J3dTintedTextureSumResult::MissingTextureCoordinate;
    }
    const auto boundTexture = [](const PictureTexture& texture) {
        return texture.resource != 0 && texture.width != 0 && texture.height != 0;
    };
    if (firstStage.textureCoordinate != 0 || secondStage.textureCoordinate != 1 ||
        j3d_texture_number_for_map(state, firstStage.textureMap) == 0xFFFFU ||
        j3d_texture_number_for_map(state, secondStage.textureMap) == 0xFFFFU ||
        !boundTexture(first) || !boundTexture(second)) {
        return J3dTintedTextureSumResult::UnsupportedTextureBinding;
    }
    if (!state.hasTevColors) {
        return J3dTintedTextureSumResult::MissingRegisterColor;
    }
    ModelRasterPolicy raster{};
    if (classify_j3d_raster_policy(state, raster) != J3dRasterPolicyResult::Success) {
        return J3dTintedTextureSumResult::UnsupportedRasterPolicy;
    }

    const Color firstTint = selected_constant(state, firstStage.konstColorSelection);
    const Color secondTint = selected_constant(state, secondStage.konstColorSelection);
    material.firstTexture = first;
    material.secondTexture = second;
    // Opacity comes from a colour register rather than from either constant: the first stage
    // multiplies that register's alpha by its image's, and the second multiplies in its own.
    material.firstTint = {firstTint.r, firstTint.g, firstTint.b,
                          color_from_s10(state.tevColorsS10[0]).a};
    material.secondTint = {secondTint.r, secondTint.g, secondTint.b, 0.0F};
    material.raster = raster;
    return J3dTintedTextureSumResult::Success;
}

} // namespace sb::native_render
