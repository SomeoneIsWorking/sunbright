#include <sunbright/native_render/j3d_interpolated_register_material.h>

#include <cassert>
#include <cmath>

namespace {

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001F;
}

// GMSE01's material at 0x80fa490c, read from the running game: one image choosing between two
// colour registers, offset by a colour constant's alpha, then averaged with a second image at an
// authored five eighths. Its lit channel is enabled and no stage reads it.
sb::native_render::J3dMaterialState material_state() {
    using namespace sb::native_render;
    return {
        .supportedColorBlock = true,
        .cullMode = static_cast<std::uint8_t>(ModelCullMode::Back),
        .lightingEnabled = true,
        .colorChannelCount = 1,
        .colorChannelControl = 0x0706,
        .alphaChannelControl = 0x0700,
        .materialColorRgba8 = 0xFFFFFFFF,
        .textureCoordinateCount = 2,
        .tevBlockType = 0x54564232U,
        .supportedTevBlock = true,
        .tevStageCount = 2,
        .textureBindings = {j3d_texture_binding(1), j3d_texture_binding(2)},
        .tevStages = {j3d_tev_stage(0, 0, 4, {0xC0, 0x08, 0x24, 0x8E, 0xC1, 0x08, 0xF0, 0xF0}, 0x1C,
                                    0x1C),
                      j3d_tev_stage(1, 1, 4, {0xC2, 0x39, 0xF8, 0xE0, 0xC3, 0x08, 0xF8, 0x00}, 0x03,
                                    0x05)},
        .hasTevColors = true,
        .tevColorsS10 = {{{255, 128, 0, 192}, {0, 64, 255, 255}}},
        .konstColorRgba8 = {0x00000040},
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
        .hasNormal = true,
    };
}

} // namespace

int main() {
    using namespace sb::native_render;

    const PictureTexture blend{.resource = 7, .width = 64, .height = 64};
    const PictureTexture detail{.resource = 8, .width = 32, .height = 32};

    J3dMaterialState state = material_state();
    InterpolatedRegisterMaterial material{};
    assert(classify_j3d_interpolated_register_material(state, blend, detail, material) ==
           J3dInterpolatedRegisterResult::Success);
    assert(material.blendTexture == blend);
    assert(material.detailTexture == detail);
    assert(near(material.lowerColor.r, 1.0F));
    assert(near(material.lowerColor.g, 128.0F / 255.0F));
    assert(near(material.lowerColor.b, 0.0F));
    // The lower register's alpha is the opacity the first stage starts from.
    assert(near(material.lowerColor.a, 192.0F / 255.0F));
    assert(near(material.upperColor.b, 1.0F));
    // The offset is the selected constant's alpha broadcast to colour, not its colour.
    assert(near(material.colorOffset, 64.0F / 255.0F));
    assert(near(material.detailWeight, 0.625F));
    // The second stage adds three eighths of the alpha it already has rather than replacing it.
    assert(near(material.alphaGain, 1.375F));
    assert(material.raster.blend == ModelBlendMode::Additive);

    InterpolatedRegisterMaterial refused{};
    // The detail weight is read from the selection, so a different fraction is a different
    // material rather than the same one with a transcribed constant.
    J3dMaterialState otherWeight = material_state();
    otherWeight.tevStages[1].konstColorSelection = 0x04;
    assert(classify_j3d_interpolated_register_material(otherWeight, blend, detail, refused) ==
           J3dInterpolatedRegisterResult::UnsupportedColorProgram);

    // A selection naming a whole constant colour rather than its alpha is a different offset.
    J3dMaterialState colorSelection = material_state();
    colorSelection.tevStages[0].konstColorSelection = 0x0C;
    assert(classify_j3d_interpolated_register_material(colorSelection, blend, detail, refused) ==
           J3dInterpolatedRegisterResult::UnsupportedColorProgram);

    // Without the colour registers there is nothing to interpolate between.
    J3dMaterialState noRegisters = material_state();
    noRegisters.hasTevColors = false;
    assert(classify_j3d_interpolated_register_material(noRegisters, blend, detail, refused) ==
           J3dInterpolatedRegisterResult::MissingRegisterColor);

    J3dMaterialState singleStage = material_state();
    singleStage.tevStageCount = 1;
    assert(classify_j3d_interpolated_register_material(singleStage, blend, detail, refused) ==
           J3dInterpolatedRegisterResult::UnsupportedStageCount);

    return 0;
}
