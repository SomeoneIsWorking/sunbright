#include <sunbright/native_render/j3d_tinted_layered_material.h>

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
        .materialColor1Rgba8 = 0xFF8040FF,
        .ambientColor1Rgba8 = 0,
        .textureCoordinateCount = 2,
        .tevBlockType = 0x54564232U,
        .supportedTevBlock = true,
        .tevStageCount = 2,
        .textureBindings = {j3d_texture_binding(1), j3d_texture_binding(2)},
        .tevStages = {j3d_tev_stage(1, 1, 4, {0xC0, 0x0A, 0x8A, 0xE2, 0xC1, 0x08, 0xFF, 0xD0},
                                    0x03),
                      j3d_tev_stage(0, 0, 5, {0xC2, 0x0A, 0x0A, 0xE8, 0xC3, 0x08, 0xFF, 0x80},
                                    0x04)},
        .tevColor0S10 = {194, 178, 206, 161},
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
    const PictureTexture baseTexture{.resource = 10, .width = 32, .height = 32};
    const PictureTexture detailTexture{.resource = 11, .width = 16, .height = 16};
    const ModelLightingContext lighting{
        .pointLights = {{{.position = {0, 0, 1}, .color = {1, 0, 0, 1}},
                         {.position = {0, 0, 1}, .color = {0, 1, 0, 1}}}},
        .pointLightCount = 2,
        .specular = {.directionToLight = {0, 0, 1},
                     .color = {0.5F, 0.5F, 0.5F, 1},
                     .shininess = 50},
    };
    LitTintedLayeredSpecularMaterial material{};
    assert(classify_j3d_tinted_layered_material(state, baseTexture, detailTexture, lighting,
                                                material) ==
           J3dTintedLayeredMaterialResult::Success);
    assert(material.baseTexture == baseTexture);
    assert(material.detailTexture == detailTexture);
    assert(near(material.effectColor.r, 194.0F / 255.0F));
    assert(near(material.detailWeight, 3.0F / 8.0F));
    assert(near(material.layerWeight, 0.5F));
    assert(material.lighting.pointLightCount == 2);
    assert(near(material.lighting.specular.color.g, 0.5F * (128.0F / 255.0F)));
    assert(material.raster.blend == ModelBlendMode::SourceAlpha);
    assert(material.raster.depthWrite);

    const ModelDraw draw{
        .instance = 1,
        .mesh = {.resource = 2, .revision = 1, .vertexCount = 3},
        .pose = {.modelViews = {Matrix3x4{.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}},
                 .count = 1},
        .projection = {.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}},
        .material = material,
    };
    const ClipVertex transformed = transform_vertex(draw, MeshVertex{.normal = {0, 0, 1}});
    // The vertex boundary carries the texture-independent part of the first clamp and the
    // independently weighted directional highlight. The dedicated shader samples both images.
    const float base = 128.0F / 255.0F;
    assert(near(transformed.color.r, 194.0F / 255.0F + 5.0F / 8.0F * base - 0.5F));
    assert(near(transformed.color.g, 178.0F / 255.0F + 5.0F / 8.0F * base - 0.5F));
    assert(near(transformed.color.a, 128.0F / 255.0F));
    assert(near(transformed.additiveColor.r, 0.25F));
    assert(near(transformed.additiveColor.g, 0.25F * (128.0F / 255.0F)));
    assert(near(transformed.additiveColor.b, 0.25F * (64.0F / 255.0F)));
    assert(near(transformed.additiveColor.a, 0.5F));

    state.tevStages[0].program[2] ^= 1U;
    assert(classify_j3d_tinted_layered_material(state, baseTexture, detailTexture, lighting,
                                                material) ==
           J3dTintedLayeredMaterialResult::UnsupportedColorProgram);
    state = material_state();
    state.colorChannelControl = 0x0686;
    assert(classify_j3d_tinted_layered_material(state, baseTexture, detailTexture, lighting,
                                                material) ==
           J3dTintedLayeredMaterialResult::UnsupportedColorChannels);
    state = material_state();
    ModelLightingContext oneLight = lighting;
    oneLight.pointLightCount = 1;
    assert(classify_j3d_tinted_layered_material(state, baseTexture, detailTexture, oneLight,
                                                material) ==
           J3dTintedLayeredMaterialResult::MissingLightingContext);
}
