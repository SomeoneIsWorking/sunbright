#include <sunbright/native_render/j3d_tinted_texture_sum_material.h>

#include <cassert>
#include <cmath>

namespace {

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001F;
}

// GMSE01's material at 0x80e8817c, read from the running game: two stages, each scaling its own
// texture by its own colour constant, the second added to the first. Neither stage reads the
// rasterised channel, so the channels it lights are left out of what this rule gates on.
sb::native_render::J3dMaterialState material_state() {
    using namespace sb::native_render;
    return {
        .supportedColorBlock = true,
        .cullMode = static_cast<std::uint8_t>(ModelCullMode::Back),
        .colorChannelCount = 1,
        .colorChannelControl = 0x0700,
        .alphaChannelControl = 0x0700,
        .materialColorRgba8 = 0xFFFFFFFF,
        .textureCoordinateCount = 2,
        .tevBlockType = 0x54564232U,
        .supportedTevBlock = true,
        .tevStageCount = 2,
        .textureBindings = {j3d_texture_binding(1), j3d_texture_binding(2)},
        .tevStages = {j3d_tev_stage(0, 0, 4, {0xC0, 0x08, 0xFE, 0x8F, 0xC1, 0x08, 0xE6, 0x70},
                                    0x0C),
                      j3d_tev_stage(1, 1, 4, {0xC2, 0x08, 0xFE, 0x80, 0xC3, 0x08, 0xE2, 0x70},
                                    0x0D)},
        .hasTevColors = true,
        .tevColorsS10 = {{{255, 255, 255, 128}}},
        .konstColorRgba8 = {0xFF8040FF, 0x204060FF},
        .pixelEngineBlockType = 0x5045464CU,
        .hasExplicitPixelPolicy = true,
        .alphaCompare0 = 7,
        .alphaOperation = 1,
        .alphaCompare1 = 7,
        .blendMode = 1,
        .blendSourceFactor = 4,
        .blendDestinationFactor = 1,
        .blendLogicOperation = 3,
        .depthTest = true,
        .depthCompare = 3,
        .depthWrite = false,
        .hasVertexColor = true,
        .hasNormal = true,
    };
}

} // namespace

int main() {
    using namespace sb::native_render;

    const PictureTexture first{.resource = 7, .width = 64, .height = 64};
    const PictureTexture second{.resource = 8, .width = 32, .height = 32};

    J3dMaterialState state = material_state();
    TintedTextureSumMaterial material{};
    assert(classify_j3d_tinted_texture_sum_material(state, first, second, material) ==
           J3dTintedTextureSumResult::Success);
    assert(material.firstTexture == first);
    assert(material.secondTexture == second);
    assert(near(material.firstTint.r, 1.0F));
    assert(near(material.firstTint.g, 128.0F / 255.0F));
    assert(near(material.firstTint.b, 64.0F / 255.0F));
    // Opacity is the colour register's alpha, not either constant's.
    assert(near(material.firstTint.a, 128.0F / 255.0F));
    assert(near(material.secondTint.r, 32.0F / 255.0F));
    assert(near(material.secondTint.g, 64.0F / 255.0F));
    assert(near(material.secondTint.b, 96.0F / 255.0F));
    assert(material.raster.blend == ModelBlendMode::Additive);

    // Each stage names its own constant, so swapping the selections swaps the tints rather than
    // leaving both layers reading the same slot.
    J3dMaterialState swapped = material_state();
    swapped.tevStages[0].konstColorSelection = 0x0D;
    swapped.tevStages[1].konstColorSelection = 0x0C;
    TintedTextureSumMaterial swappedMaterial{};
    assert(classify_j3d_tinted_texture_sum_material(swapped, first, second, swappedMaterial) ==
           J3dTintedTextureSumResult::Success);
    assert(near(swappedMaterial.firstTint.r, 32.0F / 255.0F));
    assert(near(swappedMaterial.secondTint.r, 1.0F));

    TintedTextureSumMaterial refused{};
    // A selection naming an authored fraction is a different program: nothing here knows what the
    // stage would then scale its texture by.
    J3dMaterialState fractionSelection = material_state();
    fractionSelection.tevStages[1].konstColorSelection = 0x03;
    assert(classify_j3d_tinted_texture_sum_material(fractionSelection, first, second, refused) ==
           J3dTintedTextureSumResult::UnsupportedColorProgram);

    // The second stage adds; a second stage that multiplied would be a different material.
    J3dMaterialState multiplying = material_state();
    multiplying.tevStages[1].program = {0xC2, 0x08, 0xFE, 0x8F, 0xC3, 0x08, 0xE2, 0x70};
    assert(classify_j3d_tinted_texture_sum_material(multiplying, first, second, refused) ==
           J3dTintedTextureSumResult::UnsupportedColorProgram);

    // Without the colour registers there is no opacity to publish, so it refuses rather than
    // inventing one.
    J3dMaterialState noRegisters = material_state();
    noRegisters.hasTevColors = false;
    assert(classify_j3d_tinted_texture_sum_material(noRegisters, first, second, refused) ==
           J3dTintedTextureSumResult::MissingRegisterColor);

    J3dMaterialState singleStage = material_state();
    singleStage.tevStageCount = 1;
    assert(classify_j3d_tinted_texture_sum_material(singleStage, first, second, refused) ==
           J3dTintedTextureSumResult::UnsupportedStageCount);

    return 0;
}
