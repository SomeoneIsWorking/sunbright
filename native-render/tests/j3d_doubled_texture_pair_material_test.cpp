#include <sunbright/native_render/j3d_doubled_texture_pair_material.h>

#include <cassert>
#include <cmath>

namespace {

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001F;
}

// GMSE01's material at 0x80e85ac0, read from the running game: unlit, two stages, the first
// multiplying its texture by the rasterised channel and the second multiplying that by a second
// texture and doubling it. Its colour channel takes the material register and its alpha the vertex.
sb::native_render::J3dMaterialState raster_tinted_state() {
    using namespace sb::native_render;
    return {
        .supportedColorBlock = true,
        .cullMode = static_cast<std::uint8_t>(ModelCullMode::Back),
        .colorChannelCount = 1,
        .colorChannelControl = 0x0700,
        .alphaChannelControl = 0x0701,
        .materialColorRgba8 = 0x8040C0FF,
        .textureCoordinateCount = 2,
        .tevBlockType = 0x54564232U,
        .supportedTevBlock = true,
        .tevStageCount = 2,
        .textureBindings = {j3d_texture_binding(1), j3d_texture_binding(2)},
        .tevStages = {j3d_tev_stage(0, 0, 4, {0xC0, 0x00, 0xF8, 0xAF, 0xC1, 0x00, 0xF2, 0xF0}, 0x1F,
                                    0x1F),
                      j3d_tev_stage(1, 1, 255, {0xC2, 0x18, 0xF8, 0x0F, 0xC3, 0x18, 0xF0, 0x70},
                                    0x1F, 0x1F)},
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
    };
}

// GMSE01's material at 0x80fa4c00: the same pair, tinted by a colour constant instead. No stage
// reads the rasterised channel, so its lit channel's output goes nowhere, and opacity comes from
// the constant's alpha times a colour register's rather than from the textures.
sb::native_render::J3dMaterialState constant_tinted_state() {
    using namespace sb::native_render;
    J3dMaterialState state = raster_tinted_state();
    state.lightingEnabled = true;
    state.colorChannelControl = 0x0706;
    state.alphaChannelControl = 0x0700;
    state.hasNormal = true;
    state.tevStages[0] =
        j3d_tev_stage(0, 0, 4, {0xC0, 0x08, 0xFE, 0x8F, 0xC1, 0x08, 0xF8, 0xF0}, 0x0C, 0x1C);
    state.tevStages[1] =
        j3d_tev_stage(1, 1, 4, {0xC2, 0x10, 0xF0, 0x8F, 0xC3, 0x00, 0xFF, 0x80}, 0x0C, 0x1C);
    state.hasTevColors = true;
    state.tevColorsS10 = {{{255, 255, 255, 128}}};
    state.konstColorRgba8 = {0x8040C080};
    return state;
}

} // namespace

int main() {
    using namespace sb::native_render;

    const PictureTexture base{.resource = 7, .width = 64, .height = 64};
    const PictureTexture detail{.resource = 8, .width = 32, .height = 32};

    J3dMaterialState state = raster_tinted_state();
    DoubledTexturePairMaterial material{};
    assert(classify_j3d_doubled_texture_pair_material(state, base, detail, material) ==
           J3dDoubledTexturePairResult::Success);
    assert(material.baseTexture == base);
    assert(material.detailTexture == detail);
    assert(material.alphaMode == ModelPairAlphaMode::DoubledTextureProduct);
    // The colour channel names the material register and the alpha channel the vertex, so they are
    // published separately rather than one flag standing for both.
    assert(!material.tintRgbFromVertexColor);
    assert(material.tintAlphaFromVertexColor);
    assert(near(material.tint.r, 128.0F / 255.0F));
    assert(near(material.tint.g, 64.0F / 255.0F));
    assert(near(material.tint.b, 192.0F / 255.0F));
    assert(material.raster.blend == ModelBlendMode::Additive);
    assert(!material.raster.depthWrite);
    assert(material.raster.depthCompare == ModelDepthCompare::LessOrEqual);

    // The rasterised channel decides what this spelling draws, so lighting it would change the
    // output and the family must refuse rather than ignore it.
    J3dMaterialState lit = raster_tinted_state();
    lit.lightingEnabled = true;
    DoubledTexturePairMaterial refused{};
    assert(classify_j3d_doubled_texture_pair_material(lit, base, detail, refused) ==
           J3dDoubledTexturePairResult::UnsupportedColorChannels);

    // A vertex-sourced channel with no vertex colour to read is refused, not defaulted.
    J3dMaterialState missingVertexColor = raster_tinted_state();
    missingVertexColor.hasVertexColor = false;
    assert(classify_j3d_doubled_texture_pair_material(missingVertexColor, base, detail, refused) ==
           J3dDoubledTexturePairResult::MissingVertexColor);

    state = constant_tinted_state();
    DoubledTexturePairMaterial constant{};
    assert(classify_j3d_doubled_texture_pair_material(state, base, detail, constant) ==
           J3dDoubledTexturePairResult::Success);
    assert(constant.alphaMode == ModelPairAlphaMode::Constant);
    assert(!constant.tintRgbFromVertexColor);
    assert(!constant.tintAlphaFromVertexColor);
    assert(near(constant.tint.r, 128.0F / 255.0F));
    assert(near(constant.tint.g, 64.0F / 255.0F));
    assert(near(constant.tint.b, 192.0F / 255.0F));
    // 128/255 from the constant's alpha, halved again by the colour register's 128/255.
    assert(near(constant.tint.a, (128.0F / 255.0F) * (128.0F / 255.0F)));

    // The constant spelling names its register by selection, so a different constant is a
    // different tint rather than the same one read from a fixed slot.
    J3dMaterialState secondConstant = constant_tinted_state();
    secondConstant.konstColorRgba8 = {0x8040C080, 0x204060FF};
    secondConstant.tevStages[0] =
        j3d_tev_stage(0, 0, 4, {0xC0, 0x08, 0xFE, 0x8F, 0xC1, 0x08, 0xF8, 0xF0}, 0x0D, 0x1D);
    DoubledTexturePairMaterial second{};
    assert(classify_j3d_doubled_texture_pair_material(secondConstant, base, detail, second) ==
           J3dDoubledTexturePairResult::Success);
    assert(near(second.tint.r, 32.0F / 255.0F));
    assert(near(second.tint.a, 128.0F / 255.0F));

    // A selection naming an authored fraction rather than a constant register is a different
    // program, and this family does not know what its first stage would then multiply.
    J3dMaterialState fractionSelection = constant_tinted_state();
    fractionSelection.tevStages[0] =
        j3d_tev_stage(0, 0, 4, {0xC0, 0x08, 0xFE, 0x8F, 0xC1, 0x08, 0xF8, 0xF0}, 0x03, 0x03);
    assert(classify_j3d_doubled_texture_pair_material(fractionSelection, base, detail, refused) ==
           J3dDoubledTexturePairResult::UnsupportedColorProgram);

    // One stage is not a pair, and neither is a pair whose second coordinate is the first's.
    J3dMaterialState singleStage = raster_tinted_state();
    singleStage.tevStageCount = 1;
    assert(classify_j3d_doubled_texture_pair_material(singleStage, base, detail, refused) ==
           J3dDoubledTexturePairResult::UnsupportedStageCount);
    J3dMaterialState sharedCoordinate = raster_tinted_state();
    sharedCoordinate.tevStages[1].textureCoordinate = 0;
    assert(classify_j3d_doubled_texture_pair_material(sharedCoordinate, base, detail, refused) ==
           J3dDoubledTexturePairResult::UnsupportedTextureBinding);

    return 0;
}
