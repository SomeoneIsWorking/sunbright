#include <sunbright/native_render/j3d_effect_material.h>

#include <cassert>
#include <cmath>

namespace {

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001F;
}

sb::native_render::J3dMaterialState material_state() {
    using namespace sb::native_render;
    J3dMaterialState state{
        .supportedColorBlock = true,
        .cullMode = static_cast<std::uint8_t>(ModelCullMode::Back),
        .lightingEnabled = true,
        .colorChannelCount = 1,
        .colorChannelControl = 0x0706,
        .alphaChannelControl = 0x0700,
        .textureCoordinateCount = 1,
        .supportedTevBlock = true,
        .tevStageCount = 1,
        .textureBindings = {j3d_texture_binding(2)},
        .tevStages = {j3d_tev_stage(0, 0, 4, {0xC0, 0x08, 0xFE, 0x8F, 0xC1, 0x08, 0xE6, 0x70}, 0x0C,
                                    0x05)},
        .hasTevColors = true,
        .tevColorsS10 = {{{12, 24, 36, 96}, {0, 0, 0, 0}, {0, 0, 0, 0}}},
        .konstColorRgba8 = {0x804020FF, 0, 0, 0},
        .pixelEngineBlockType = 0x50454F50U,
        .hasNormal = true,
    };
    return state;
}

} // namespace

int main() {
    using namespace sb::native_render;
    const PictureTexture texture{.resource = 9, .width = 64, .height = 64};
    J3dMaterialState state = material_state();
    TexturedEffectMaterial material{};
    assert(classify_j3d_effect_material(state, texture, material) ==
           J3dEffectMaterialResult::Success);
    assert(material.texture == texture);
    assert(near(material.modulation.r, 128.0F / 255.0F));
    assert(near(material.modulation.g, 64.0F / 255.0F));
    assert(near(material.modulation.b, 32.0F / 255.0F));
    assert(near(material.modulation.a, 96.0F / 255.0F));

    state = material_state();
    state.tevStages[0].program = {0xC0, 0x08, 0xF2, 0x8F, 0xC1, 0x38, 0xE6, 0x70};
    assert(classify_j3d_effect_material(state, texture, material) ==
           J3dEffectMaterialResult::Success);
    assert(near(material.modulation.r, 12.0F / 255.0F));
    assert(near(material.modulation.g, 24.0F / 255.0F));
    assert(near(material.modulation.b, 36.0F / 255.0F));
    assert(near(material.modulation.a, 48.0F / 255.0F));

    state.hasExplicitPixelPolicy = true;
    state.pixelEngineBlockType = 0x5045464CU;
    state.alphaCompare0 = 7;
    state.alphaCompare1 = 7;
    state.alphaOperation = 1;
    state.blendMode = 1;
    state.blendSourceFactor = 4;
    state.blendDestinationFactor = 1;
    state.depthTest = false;
    state.depthCompare = 3;
    state.depthWrite = false;
    assert(classify_j3d_effect_material(state, texture, material) ==
           J3dEffectMaterialResult::Success);
    assert(material.raster.blend == ModelBlendMode::Additive);
    assert(!material.raster.depthTest);
    state.alphaCompare0 = 4;
    state.alphaReference0 = 64;
    state.alphaOperation = 0;
    assert(classify_j3d_effect_material(state, texture, material) ==
           J3dEffectMaterialResult::Success);
    assert(material.raster.alphaTest == ModelAlphaTest::GreaterThan64);
    state.depthTest = true;
    assert(classify_j3d_effect_material(state, texture, material) ==
           J3dEffectMaterialResult::UnsupportedRasterPolicy);
    state.depthTest = false;

    state.tevStages[0].program[2] ^= 1U;
    assert(classify_j3d_effect_material(state, texture, material) ==
           J3dEffectMaterialResult::UnsupportedColorProgram);
    state = material_state();
    state.hasTevColors = false;
    assert(classify_j3d_effect_material(state, texture, material) ==
           J3dEffectMaterialResult::MissingTevColor);
}
