#include <sunbright/native_render/j3d_masked_doubled_texture_material.h>

#include <cassert>
#include <cmath>

namespace {

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001F;
}

// GMSE01's material at 0x80ed7738, read from the running game: two stages, both multiplying their
// texture by the rasterised channel, the second doubling it and overwriting the first's colour
// while multiplying its alpha in.
sb::native_render::J3dMaterialState material_state() {
    using namespace sb::native_render;
    return {
        .supportedColorBlock = true,
        .cullMode = static_cast<std::uint8_t>(ModelCullMode::Back),
        .colorChannelCount = 1,
        .colorChannelControl = 0x0700,
        .alphaChannelControl = 0x0700,
        .materialColorRgba8 = 0xC08040A0,
        .textureCoordinateCount = 2,
        .tevBlockType = 0x54564232U,
        .supportedTevBlock = true,
        .tevStageCount = 2,
        .textureBindings = {j3d_texture_binding(1), j3d_texture_binding(2)},
        .tevStages = {j3d_tev_stage(0, 0, 4, {0xC0, 0x08, 0xF8, 0xAF, 0xC1, 0x08, 0xF2, 0xF0}, 0x0C,
                                    0x1C),
                      j3d_tev_stage(1, 1, 4, {0xC2, 0x18, 0xF8, 0xAF, 0xC3, 0x00, 0xF0, 0x70}, 0x0C,
                                    0x1C)},
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
    };
}

} // namespace

int main() {
    using namespace sb::native_render;

    const PictureTexture opacity{.resource = 7, .width = 64, .height = 64};
    const PictureTexture color{.resource = 8, .width = 32, .height = 32};

    J3dMaterialState state = material_state();
    MaskedDoubledTextureMaterial material{};
    assert(classify_j3d_masked_doubled_texture_material(state, opacity, color, material) ==
           J3dMaskedDoubledTextureResult::Success);
    assert(material.opacityTexture == opacity);
    assert(material.colorTexture == color);
    assert(near(material.tint.r, 192.0F / 255.0F));
    assert(near(material.tint.g, 128.0F / 255.0F));
    assert(near(material.tint.b, 64.0F / 255.0F));
    assert(near(material.tint.a, 160.0F / 255.0F));
    assert(material.raster.blend == ModelBlendMode::Additive);
    assert(!material.raster.depthWrite);

    MaskedDoubledTextureMaterial refused{};
    // Both stages read the rasterised channel, so lighting it changes what is drawn.
    J3dMaterialState lit = material_state();
    lit.lightingEnabled = true;
    assert(classify_j3d_masked_doubled_texture_material(lit, opacity, color, refused) ==
           J3dMaskedDoubledTextureResult::Lighting);

    // A second stage that multiplies what the first produced is the doubled pair, not this
    // material: there the first image's colour survives, here it is overwritten.
    J3dMaterialState combining = material_state();
    combining.tevStages[1].program = {0xC2, 0x18, 0xF8, 0x0F, 0xC3, 0x18, 0xF0, 0x70};
    assert(classify_j3d_masked_doubled_texture_material(combining, opacity, color, refused) ==
           J3dMaskedDoubledTextureResult::UnsupportedColorProgram);

    // A vertex-sourced channel is a different material; nothing here reads a vertex colour.
    J3dMaterialState vertexSourced = material_state();
    vertexSourced.colorChannelControl = 0x0701;
    assert(classify_j3d_masked_doubled_texture_material(vertexSourced, opacity, color, refused) ==
           J3dMaskedDoubledTextureResult::UnsupportedColorChannels);

    // One coordinate for both stages would sample one image twice.
    J3dMaterialState sharedCoordinate = material_state();
    sharedCoordinate.tevStages[1].textureCoordinate = 0;
    assert(
        classify_j3d_masked_doubled_texture_material(sharedCoordinate, opacity, color, refused) ==
        J3dMaskedDoubledTextureResult::UnsupportedTextureBinding);

    J3dMaterialState singleStage = material_state();
    singleStage.tevStageCount = 1;
    assert(classify_j3d_masked_doubled_texture_material(singleStage, opacity, color, refused) ==
           J3dMaskedDoubledTextureResult::UnsupportedStageCount);

    return 0;
}
