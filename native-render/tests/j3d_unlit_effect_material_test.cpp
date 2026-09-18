#include <sunbright/native_render/j3d_unlit_effect_material.h>

#include <cassert>
#include <cmath>

namespace {

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001F;
}

// GMSE01's material at 0x80d3a7e8, read from the running game: unlit, one stage taking its colour
// from a register and its alpha from the raster, with the opaque pixel policy the title authors.
sb::native_render::J3dMaterialState material_state() {
    using namespace sb::native_render;
    return {
        .supportedColorBlock = true,
        .cullMode = static_cast<std::uint8_t>(ModelCullMode::Back),
        .colorChannelCount = 1,
        .colorChannelControl = 0x0701,
        .alphaChannelControl = 0x0700,
        .materialColorRgba8 = 0xFFFFFF80,
        .textureCoordinateCount = 1,
        .tevBlockType = 0x54564231U,
        .supportedTevBlock = true,
        .tevStageCount = 1,
        .textureBindings = {j3d_texture_binding(1)},
        .tevStages = {j3d_tev_stage(0, 0, 4, {0xC0, 0x08, 0xF8, 0x2F, 0xC1, 0x08, 0xF2, 0xF0},
                                    0x0C)},
        .hasTevColors = true,
        .tevColorsS10 = {{{255, 128, 64, 255}}},
        .pixelEngineBlockType = 0x5045464CU,
        .hasExplicitPixelPolicy = true,
        .alphaCompare0 = 7,
        .alphaOperation = 1,
        .alphaCompare1 = 7,
        .blendMode = 0,
        .blendSourceFactor = 1,
        .blendDestinationFactor = 0,
        .blendLogicOperation = 3,
        .depthTest = true,
        .depthCompare = 3,
        .depthWrite = true,
        .hasVertexColor = true,
    };
}

} // namespace

int main() {
    using namespace sb::native_render;

    J3dMaterialState state = material_state();
    const PictureTexture texture{.resource = 7, .width = 64, .height = 64};
    TexturedEffectMaterial material{};
    assert(classify_j3d_unlit_effect_material(state, texture, material) ==
           J3dUnlitEffectResult::Success);
    assert(material.texture == texture);
    assert(material.textureCoordinates == ModelTextureCoordinates::Primary);
    assert(material.alphaMode == ModelTextureAlphaMode::MultiplyTexture);
    // The colour comes from the register the stage names, and the opacity from the material colour
    // the alpha channel publishes as the raster alpha.
    assert(near(material.modulation.r, 1.0F));
    assert(near(material.modulation.g, 128.0F / 255.0F));
    assert(near(material.modulation.b, 64.0F / 255.0F));
    assert(near(material.modulation.a, 128.0F / 255.0F));
    assert(material.additive == Color{});
    assert(material.raster.cull == ModelCullMode::Back);
    assert(material.raster.depthWrite);

    // Nothing reads the colour channel's output, so both authored spellings of it mean the same
    // material. This is exact rather than lenient: the program's only raster input is alpha.
    state.colorChannelControl = 0x0700;
    assert(classify_j3d_unlit_effect_material(state, texture, material) ==
           J3dUnlitEffectResult::Success);
    state.colorChannelControl = 0x0702;
    assert(classify_j3d_unlit_effect_material(state, texture, material) ==
           J3dUnlitEffectResult::UnsupportedColorChannels);

    // Each gate refuses on its own.
    state = material_state();
    state.lightingEnabled = true;
    assert(classify_j3d_unlit_effect_material(state, texture, material) ==
           J3dUnlitEffectResult::Lighting);
    state = material_state();
    state.alphaChannelControl = 0x0701;
    assert(classify_j3d_unlit_effect_material(state, texture, material) ==
           J3dUnlitEffectResult::UnsupportedColorChannels);
    state = material_state();
    state.tevStages[0].program[3] = 0xAF;
    assert(classify_j3d_unlit_effect_material(state, texture, material) ==
           J3dUnlitEffectResult::UnsupportedColorProgram);
    state = material_state();
    state.hasTevColors = false;
    assert(classify_j3d_unlit_effect_material(state, texture, material) ==
           J3dUnlitEffectResult::MissingRegisterColor);
    state = material_state();
    state.tevStageCount = 2;
    assert(classify_j3d_unlit_effect_material(state, texture, material) ==
           J3dUnlitEffectResult::UnsupportedStageCount);
    state = material_state();
    assert(classify_j3d_unlit_effect_material(state, PictureTexture{}, material) ==
           J3dUnlitEffectResult::UnsupportedTextureBinding);
    state.depthCompare = 9;
    assert(classify_j3d_unlit_effect_material(state, texture, material) ==
           J3dUnlitEffectResult::UnsupportedRasterPolicy);
    return 0;
}
