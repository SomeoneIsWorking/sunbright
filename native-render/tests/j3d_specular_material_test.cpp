#include <sunbright/native_render/j3d_specular_material.h>

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
        .usesMaterialAmbient = false,
        .cullMode = static_cast<std::uint8_t>(ModelCullMode::Back),
        .lightingEnabled = true,
        .colorChannelCount = 2,
        .colorChannelControl = 0x070E,
        .alphaChannelControl = 0x0700,
        .colorChannelControl1 = 0x0212,
        .alphaChannelControl1 = 0x0400,
        .materialColorRgba8 = 0x808080FF,
        .ambientColorRgba8 = 0x000000FF,
        .materialColor1Rgba8 = 0xFFFFFFFF,
        .ambientColor1Rgba8 = 0,
        .textureCoordinateCount = 1,
        .tevBlockType = 0x54564232U,
        .supportedTevBlock = true,
        .tevStageCount = 2,
        .textureNumber0 = 3,
        .textureCoordinate0 = 0,
        .textureMap0 = 0,
        .colorChannel0 = 4,
        .tevStage0 = {0xC0, 0x38, 0xF8, 0xAF, 0xC1, 0x08, 0xF2, 0xF0},
        .textureNumber1 = 0xFFFF,
        .textureCoordinate1 = 0xFF,
        .textureMap1 = 0xFF,
        .colorChannel1 = 5,
        .tevStage1 = {0xC2, 0x18, 0xEC, 0x0A, 0xC3, 0x00, 0xBF, 0xF1},
        .konstColorRgba8 = {0x1A1A1AFF, 0, 0, 0},
        .konstColorSelection0 = 0x0C,
        .konstColorSelection1 = 0x0C,
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
        .hasNormal = true,
    };
}

} // namespace

int main() {
    using namespace sb::native_render;

    J3dMaterialState state = material_state();
    const PictureTexture texture{.resource = 5, .width = 32, .height = 32};
    const ModelLightingContext lighting{
        .pointLights = {{{.position = {0, 0, 1}, .color = {1, 1, 1, 1}}}},
        .pointLightCount = 1,
        .specular = {.directionToLight = {0, 0, 1},
                     .color = {0.25F, 0.25F, 0.25F, 1},
                     .shininess = 50},
    };
    TintedSpecularTexturedMaterial material{};
    assert(classify_j3d_specular_textured_material(state, texture, lighting, material) ==
           J3dSpecularTexturedResult::Success);
    assert(material.baseColor == color_from_rgba8(state.materialColorRgba8));
    assert(material.ambientColor == lighting.ambientColor);
    assert(material.tintColor == color_from_rgba8(state.konstColorRgba8[0]));
    assert(material.raster.cull == ModelCullMode::Back);
    state.usesMaterialAmbient = true;
    assert(classify_j3d_specular_textured_material(state, texture, lighting, material) ==
           J3dSpecularTexturedResult::Success);
    assert(material.ambientColor == color_from_rgba8(state.ambientColorRgba8));
    state.usesMaterialAmbient = false;

    ModelDraw draw{.mesh = {.resource = 1, .revision = 1, .vertexCount = 3},
                   .modelView = {.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}},
                   .projection = {.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}},
                   .material = material};
    const MeshVertex vertex{.normal = {0, 0, 1}};
    const ClipVertex transformed = transform_vertex(draw, vertex);
    const float tint = 0x1A / 255.0F;
    assert(near(transformed.color.r, (0x80 / 255.0F) * (1.0F - tint)));
    assert(near(transformed.additiveColor.r, 2.0F * (tint + 0.25F)));
    assert(near(transformed.color.a, 0.0F));
    assert(near(transformed.additiveColor.a, 1.0F));

    state.colorChannelControl1 = 0x0210;
    assert(classify_j3d_specular_textured_material(state, texture, lighting, material) ==
           J3dSpecularTexturedResult::UnsupportedColorChannels);
    state = material_state();
    state.konstColorSelection1 = 0x0D;
    assert(classify_j3d_specular_textured_material(state, texture, lighting, material) ==
           J3dSpecularTexturedResult::UnsupportedColorProgram);
    state = material_state();
    state.hasNormal = false;
    assert(classify_j3d_specular_textured_material(state, texture, lighting, material) ==
           J3dSpecularTexturedResult::MissingNormal);
    state = material_state();
    state.tevStage1[0] ^= 1;
    assert(classify_j3d_specular_textured_material(state, texture, lighting, material) ==
           J3dSpecularTexturedResult::UnsupportedColorProgram);

    ModelLightingContext invalidLighting = lighting;
    invalidLighting.specular.shininess = 0;
    state = material_state();
    assert(classify_j3d_specular_textured_material(state, texture, invalidLighting, material) ==
           J3dSpecularTexturedResult::MissingLightingContext);
}
