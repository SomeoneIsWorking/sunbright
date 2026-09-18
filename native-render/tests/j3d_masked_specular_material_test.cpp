#include <sunbright/native_render/j3d_masked_specular_material.h>

#include <cassert>
#include <cmath>

namespace {

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001F;
}

// The material GMSE01 authors at 0x80d06840: two colour channels, the first signed-diffuse from one
// light with a lit alpha, the second the directional highlight. The stages were read back from the
// running game and decoded with tools/re/tev_decode.py.
sb::native_render::J3dMaterialState material_state() {
    using namespace sb::native_render;
    return {
        .supportedColorBlock = true,
        .usesMaterialAmbient = true,
        .cullMode = static_cast<std::uint8_t>(ModelCullMode::Back),
        .lightingEnabled = true,
        .colorChannelCount = 2,
        .colorChannelControl = 0x0686,
        .alphaChannelControl = 0x0706,
        .colorChannelControl1 = 0x0212,
        .alphaChannelControl1 = 0x0400,
        .materialColorRgba8 = 0x80808080,
        .ambientColorRgba8 = 0x404040FF,
        .materialColor1Rgba8 = 0xFF8040FF,
        .ambientColor1Rgba8 = 0,
        .textureCoordinateCount = 2,
        .tevBlockType = 0x54564232U,
        .supportedTevBlock = true,
        .tevStageCount = 2,
        .textureBindings = {j3d_texture_binding(1), j3d_texture_binding(2)},
        .tevStages = {j3d_tev_stage(1, 1, 4, {0xC0, 0x09, 0xFA, 0xE8, 0xC1, 0x08, 0xFF, 0xD0},
                                    0x03),
                      j3d_tev_stage(0, 0, 5, {0xC2, 0x08, 0xA0, 0x8F, 0xC3, 0x08, 0xFF, 0x80},
                                    0x0C)},
        .pixelEngineBlockType = 0x5045464CU,
        .hasExplicitPixelPolicy = true,
        .alphaCompare0 = 7,
        .alphaOperation = 0,
        .alphaCompare1 = 7,
        .blendMode = 1,
        .blendSourceFactor = 4,
        .blendDestinationFactor = 5,
        .depthTest = true,
        .depthCompare = 3,
        .depthWrite = true,
        .hasNormal = true,
    };
}

} // namespace

int main() {
    using namespace sb::native_render;

    J3dMaterialState state = material_state();
    const PictureTexture maskTexture{.resource = 10, .width = 32, .height = 32};
    const PictureTexture detailTexture{.resource = 11, .width = 16, .height = 16};
    const ModelLightingContext lighting{
        .pointLights =
            {{{.position = {0, 0, 1}, .color = {1, 1, 1, 1}, .distanceAttenuation = {1, 0, 0}}}},
        .pointLightCount = 1,
    };
    LitMaskedSpecularMaterial material{};
    assert(classify_j3d_masked_specular_material(state, maskTexture, detailTexture, lighting,
                                                 material) == J3dMaskedSpecularResult::Success);
    // Slot 0 is the mask image because the mask stage reads map 0; the detail stage reads map 1.
    assert(material.maskTexture == maskTexture);
    assert(material.detailTexture == detailTexture);
    // The detail stage selects 0x03, five eighths, and biases by one half before clamping.
    assert(near(material.diffuseWeight, 0.625F));
    assert(near(material.detailBias, 0.5F));
    assert(!material.textureMasksAlpha);
    assert(material.lighting.pointLightCount == 1);
    assert(near(material.lighting.specular.color.r, 1.0F));
    assert(near(material.lighting.specular.color.g, 128.0F / 255.0F));

    // The second authored material differs only in its alpha: the mask image gates opacity. It has
    // to reach the same family, because a second family would be a second copy of this program.
    J3dMaterialState masksAlpha = material_state();
    masksAlpha.tevStages[1] =
        j3d_tev_stage(0, 0, 5, {0xC2, 0x08, 0xA0, 0x8F, 0xC3, 0x08, 0xF0, 0x70}, 0x0C);
    LitMaskedSpecularMaterial gated{};
    assert(classify_j3d_masked_specular_material(masksAlpha, maskTexture, detailTexture, lighting,
                                                 gated) == J3dMaskedSpecularResult::Success);
    assert(gated.textureMasksAlpha);

    // The lit alpha channel has to reach the vertex boundary as a lit value. With this light and
    // normal the illumination saturates, so the material's own alpha survives; an unlit reading
    // would agree here, which is why the dark case below is the one that discriminates.
    const ModelDraw draw{
        .instance = 1,
        .mesh = {.resource = 2, .revision = 1, .vertexCount = 3},
        .pose = {.modelViews = {Matrix3x4{.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}},
                 .count = 1},
        .projection = {.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}},
        .material = material,
    };
    assert(valid(draw));
    const ClipVertex lit = transform_vertex(draw, MeshVertex{.normal = {0, 0, 1}});
    const float base = 128.0F / 255.0F;
    assert(near(lit.color.r, 0.625F * base * 1.0F + 0.5F));
    assert(near(lit.color.a, base));
    assert(near(lit.additiveColor.a, 0.0F));

    // Facing away, the clamped alpha channel keeps only its ambient term. An alpha taken straight
    // from the material would still read 128/255 here.
    LitMaskedSpecularMaterial dark = material;
    dark.ambientColor = {0.0F, 0.0F, 0.0F, 0.25F};
    ModelDraw darkDraw = draw;
    darkDraw.material = dark;
    const ClipVertex away = transform_vertex(darkDraw, MeshVertex{.normal = {0, 0, -1}});
    assert(near(away.color.a, base * 0.25F));

    // The alpha-gated material says so at the vertex boundary, so one program serves both.
    ModelDraw gatedDraw = draw;
    gatedDraw.material = gated;
    assert(
        near(transform_vertex(gatedDraw, MeshVertex{.normal = {0, 0, 1}}).additiveColor.a, 1.0F));

    // Each gate has to be able to refuse.
    state = material_state();
    state.tevStages[0].program[2] ^= 1U;
    assert(classify_j3d_masked_specular_material(state, maskTexture, detailTexture, lighting,
                                                 material) ==
           J3dMaskedSpecularResult::UnsupportedColorProgram);
    state = material_state();
    state.tevStages[0].konstColorSelection = 0x04;
    assert(classify_j3d_masked_specular_material(state, maskTexture, detailTexture, lighting,
                                                 material) ==
           J3dMaskedSpecularResult::UnsupportedColorProgram);
    state = material_state();
    state.alphaChannelControl = 0x0700;
    assert(classify_j3d_masked_specular_material(state, maskTexture, detailTexture, lighting,
                                                 material) ==
           J3dMaskedSpecularResult::UnsupportedColorChannels);
    state = material_state();
    state.colorChannelControl1 = 0x0400;
    assert(classify_j3d_masked_specular_material(state, maskTexture, detailTexture, lighting,
                                                 material) ==
           J3dMaskedSpecularResult::UnsupportedColorChannels);
    state = material_state();
    state.tevStages[0].textureMap = 0;
    assert(classify_j3d_masked_specular_material(state, maskTexture, detailTexture, lighting,
                                                 material) ==
           J3dMaskedSpecularResult::UnsupportedTextureBindings);
    state = material_state();
    state.hasNormal = false;
    assert(classify_j3d_masked_specular_material(state, maskTexture, detailTexture, lighting,
                                                 material) ==
           J3dMaskedSpecularResult::MissingNormal);
    state = material_state();
    assert(classify_j3d_masked_specular_material(state, maskTexture, detailTexture,
                                                 ModelLightingContext{}, material) ==
           J3dMaskedSpecularResult::MissingLightingContext);
    assert(classify_j3d_masked_specular_material(state, PictureTexture{}, detailTexture, lighting,
                                                 material) ==
           J3dMaskedSpecularResult::UnsupportedTextureBindings);
    return 0;
}
