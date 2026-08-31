#include <sunbright/native_render/j3d_masked_toon_material.h>

#include <cassert>
#include <cmath>

namespace {

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001F;
}

sb::native_render::J3dMaterialState material_state() {
    using namespace sb::native_render;
    return {
        .supportedColorBlock = true,
        .usesMaterialAmbient = true,
        .cullMode = static_cast<std::uint8_t>(ModelCullMode::Back),
        .lightingEnabled = true,
        .colorChannelCount = 2,
        .colorChannelControl = 0x068E,
        .alphaChannelControl = 0x0700,
        .colorChannelControl1 = 0x0212,
        .alphaChannelControl1 = 0x0400,
        .materialColorRgba8 = 0x80808080,
        .ambientColorRgba8 = 0x404040FF,
        .materialColor1Rgba8 = 0xFFC080A0,
        .ambientColor1Rgba8 = 0,
        .textureCoordinateCount = 4,
        .tevBlockType = 0x54563136U,
        .supportedTevBlock = true,
        .tevStageCount = 5,
        .textureBindings = {j3d_texture_binding(10), j3d_texture_binding(11),
                            j3d_texture_binding(12), j3d_texture_binding(13)},
        .tevStages =
            {
                j3d_tev_stage(1, 1, 0xFF, {0xC0, 0xDB, 0x9E, 0xCF, 0xC1, 0x08, 0xFF, 0xF0}, 0x1C,
                              0x1C),
                j3d_tev_stage(0, 0, 0xFF, {0xC2, 0x08, 0xF8, 0x6F, 0xC3, 0x08, 0xFF, 0xF0}, 0x03),
                j3d_tev_stage(2, 2, 0xFF, {0xC4, 0x08, 0x8F, 0x60, 0xC5, 0x08, 0xFF, 0xF0}, 0x05,
                              0x1C),
                j3d_tev_stage(3, 3, 4, {0xC6, 0x8A, 0x8A, 0xE2, 0xC7, 0x00, 0xFF, 0xF0}, 0x03,
                              0x1C),
                j3d_tev_stage(0xFF, 0xFF, 5, {0xC8, 0x0A, 0x4A, 0xE0, 0xC9, 0x00, 0xFF, 0xD0}, 0x04,
                              0x1C),
            },
        .hasTevColors = true,
        .tevColorsS10 = {{{150, 150, 180, 161}, {157, 161, 169, 255}, {255, 255, 255, 255}}},
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
    const PictureTexture primary{.resource = 10, .width = 32, .height = 32};
    const PictureTexture mask{.resource = 11, .width = 16, .height = 16};
    const PictureTexture alternate{.resource = 12, .width = 64, .height = 32};
    const PictureTexture lightRamp{.resource = 13, .width = 8, .height = 8};
    const ModelLightingContext lighting{
        .pointLights = {{{.position = {0, 0, 1}, .color = {1, 0, 0, 1}},
                         {.position = {0, 0, 1}, .color = {0, 1, 0, 1}}}},
        .pointLightCount = 2,
        .specular = {.directionToLight = {0, 0, 1},
                     .color = {0.5F, 0.5F, 0.5F, 1},
                     .shininess = 50},
    };
    LitMaskedToonMaterial material{};
    assert(classify_j3d_masked_toon_material(state, primary, mask, alternate, lightRamp, lighting,
                                             material) == J3dMaskedToonMaterialResult::Success);
    assert(material.primaryTexture == primary);
    assert(material.maskTexture == mask);
    assert(material.alternateTexture == alternate);
    assert(material.lightRampTexture == lightRamp);
    assert(near(material.staticHighlight.r, 157.0F / 255.0F));
    assert(near(material.lightRampWeight, 3.0F / 8.0F));
    assert(near(material.staticHighlightWeight, 0.5F));
    assert(near(material.directionalHighlightWeight, 0.5F));
    assert(near(material.outputAlpha, 160.0F / 255.0F));
    assert(near(material.lighting.specular.color.g, 0.5F * (192.0F / 255.0F)));
    const ModelMaterial materialVariant{material};
    assert(material_texture_count(materialVariant) == 4);
    assert(material_texture(materialVariant, 2) ==
           &std::get<LitMaskedToonMaterial>(materialVariant).alternateTexture);

    const ModelDraw draw{
        .instance = 1,
        .mesh = {.resource = 2, .revision = 1, .vertexCount = 3},
        .pose = {.modelViews = {Matrix3x4{.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}},
                 .count = 1},
        .projection = {.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}},
        .material = material,
    };
    const ClipVertex transformed = transform_vertex(draw, MeshVertex{.normal = {0, 0, 1}});
    assert(near(transformed.color.r, 128.0F / 255.0F));
    assert(near(transformed.color.g, 128.0F / 255.0F));
    assert(near(transformed.color.a, 160.0F / 255.0F));
    assert(near(transformed.additiveColor.r, 0.5F * (157.0F / 255.0F) + 0.25F));
    assert(near(transformed.additiveColor.a, 160.0F / 255.0F));

    state.tevStages[3].program[1] ^= 1U;
    assert(classify_j3d_masked_toon_material(state, primary, mask, alternate, lightRamp, lighting,
                                             material) ==
           J3dMaskedToonMaterialResult::UnsupportedColorProgram);
    state = material_state();
    state.textureCoordinateCount = 3;
    assert(classify_j3d_masked_toon_material(state, primary, mask, alternate, lightRamp, lighting,
                                             material) ==
           J3dMaskedToonMaterialResult::MissingTextureCoordinates);
    state = material_state();
    state.textureBindings[2] = j3d_texture_binding(0xFFFFU);
    assert(classify_j3d_masked_toon_material(state, primary, mask, alternate, lightRamp, lighting,
                                             material) ==
           J3dMaskedToonMaterialResult::MissingTextureBinding);
    state = material_state();
    const PictureTexture missingAlternate{.resource = 0, .width = 64, .height = 32};
    assert(classify_j3d_masked_toon_material(state, primary, mask, missingAlternate, lightRamp,
                                             lighting, material) ==
           J3dMaskedToonMaterialResult::InvalidTextureResource);
    state = material_state();
    ModelLightingContext oneLight = lighting;
    oneLight.pointLightCount = 1;
    assert(classify_j3d_masked_toon_material(state, primary, mask, alternate, lightRamp, oneLight,
                                             material) == J3dMaskedToonMaterialResult::Success);
    ModelLightingContext noLight = lighting;
    noLight.pointLightCount = 0;
    assert(classify_j3d_masked_toon_material(state, primary, mask, alternate, lightRamp, noLight,
                                             material) ==
           J3dMaskedToonMaterialResult::MissingLightingContext);
}
