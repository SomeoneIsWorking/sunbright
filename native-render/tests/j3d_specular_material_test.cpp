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
    LitSpecularTexturedMaterial material{};
    assert(classify_j3d_specular_textured_material(state, texture, lighting, material) ==
           J3dSpecularTexturedResult::Success);
    assert(material.baseColor == color_from_rgba8(state.materialColorRgba8));
    assert(material.ambientColor == lighting.ambientColor);
    const float tint = 0x1A / 255.0F;
    assert(near(material.textureDiffuseScale.r, 1.0F - tint));
    assert(near(material.additiveColor.r, 2.0F * tint));
    assert(near(material.specularScale, 2.0F));
    assert(!material.usesVertexRgb);
    assert(material.raster.cull == ModelCullMode::Back);
    state.usesMaterialAmbient = true;
    assert(classify_j3d_specular_textured_material(state, texture, lighting, material) ==
           J3dSpecularTexturedResult::Success);
    assert(material.ambientColor == color_from_rgba8(state.ambientColorRgba8));
    state.usesMaterialAmbient = false;

    ModelDraw draw{.mesh = {.resource = 1, .revision = 1, .vertexCount = 3},
                   .pose = {.modelViews = {sb::native_render::Matrix3x4{
                                .value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}},
                            .count = 1},
                   .projection = {.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}},
                   .material = material};
    const MeshVertex vertex{.normal = {0, 0, 1}};
    const ClipVertex transformed = transform_vertex(draw, vertex);
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

    // A second reached Mario program adds the same texture multiplied by vertex-colour diffuse
    // lighting to three times the directional specular channel. Its two stages both sample the
    // same texture, but the first stage uses only the secondary raster channel for the highlight.
    state = material_state();
    state.colorChannelControl = 0x070F;
    state.materialColorRgba8 = 0x80808080;
    state.hasVertexColor = true;
    state.colorChannel0 = 5;
    state.textureCoordinate1 = 0;
    state.textureMap1 = 0;
    state.colorChannel1 = 4;
    state.tevStage0 = {0xC0, 0x18, 0xFD, 0xAA, 0xC1, 0x08, 0xF2, 0xF0};
    state.tevStage1 = {0xC2, 0x08, 0xFA, 0x80, 0xC3, 0x00, 0xBF, 0xF0};
    assert(classify_j3d_specular_textured_material(state, texture, lighting, material) ==
           J3dSpecularTexturedResult::Success);
    assert((material.textureDiffuseScale == Color{1, 1, 1, 1}));
    assert((material.additiveColor == Color{}));
    assert(near(material.specularScale, 3.0F));
    assert(material.usesVertexRgb);

    draw.material = material;
    MeshVertex vertexLit = vertex;
    vertexLit.color = {0.25F, 0.5F, 0.75F, 0.8F};
    const ClipVertex transformedLit = transform_vertex(draw, vertexLit);
    assert(near(transformedLit.color.r, 0.25F));
    assert(near(transformedLit.color.g, 0.5F));
    assert(near(transformedLit.additiveColor.r, 0.75F));
    assert(near(transformedLit.additiveColor.a, 0x80 / 255.0F));
    state.hasVertexColor = false;
    assert(classify_j3d_specular_textured_material(state, texture, lighting, material) ==
           J3dSpecularTexturedResult::MissingVertexColor);
    state.hasVertexColor = true;
    state.tevStage0[2] ^= 1U;
    assert(classify_j3d_specular_textured_material(state, texture, lighting, material) ==
           J3dSpecularTexturedResult::UnsupportedColorProgram);

    // A reached texture-free highlight uses the same high-level diffuse and directional-specular
    // inputs. Its two console stages reduce to vertex-lit colour tinted by konst colour 0, plus
    // twice the directional highlight. The secondary channel's opaque alpha makes the final
    // source opaque even though the authored full pixel block retains premultiplied blending.
    state = material_state();
    state.colorChannelControl = 0x070F;
    state.alphaChannelControl = 0x0701;
    state.textureCoordinateCount = 0;
    state.textureNumber0 = 0xFFFF;
    state.textureCoordinate0 = 0xFF;
    state.textureMap0 = 0xFF;
    state.colorChannel0 = 4;
    state.tevStage0 = {0xC0, 0x0C, 0xFA, 0xEA, 0xC1, 0x08, 0xBF, 0xF0};
    state.textureNumber1 = 0xFFFF;
    state.textureCoordinate1 = 0xFF;
    state.textureMap1 = 0xFF;
    state.colorChannel1 = 5;
    state.tevStage1 = {0xC2, 0x18, 0xF0, 0xEA, 0xC3, 0x00, 0xE3, 0x50};
    state.konstColorRgba8[0] = 0x898A90FF;
    state.konstColorSelection0 = 0x04;
    state.konstColorSelection1 = 0x0C;
    state.konstAlphaSelection0 = 0x1C;
    state.konstAlphaSelection1 = 0x1C;
    state.blendMode = 1;
    state.blendSourceFactor = 1;
    state.blendDestinationFactor = 5;
    state.blendLogicOperation = 15;
    state.depthWrite = false;
    state.hasVertexColor = true;
    LitSpecularColorMaterial colorSpecular{};
    assert(classify_j3d_specular_color_material(state, lighting, colorSpecular) ==
           J3dSpecularColorResult::Success);
    assert((colorSpecular.diffuseScale == color_from_rgba8(0x898A90FF)));
    assert(near(colorSpecular.specularScale, 2.0F));
    assert(colorSpecular.usesVertexRgb);
    assert(colorSpecular.raster.blend == ModelBlendMode::PremultipliedAlpha);
    assert(!colorSpecular.raster.depthWrite);

    draw.material = colorSpecular;
    const ClipVertex transformedColorSpecular = transform_vertex(draw, vertexLit);
    assert(near(transformedColorSpecular.color.r, vertexLit.color.r * (0x89 / 255.0F)));
    assert(near(transformedColorSpecular.color.g, vertexLit.color.g * (0x8A / 255.0F)));
    assert(near(transformedColorSpecular.additiveColor.r, 0.5F));
    assert(near(transformedColorSpecular.color.a, 0.0F));
    assert(near(transformedColorSpecular.additiveColor.a, 1.0F));
    state.tevStage1[2] ^= 1U;
    assert(classify_j3d_specular_color_material(state, lighting, colorSpecular) ==
           J3dSpecularColorResult::UnsupportedColorProgram);
    state.tevStage1[2] ^= 1U;
    state.konstAlphaSelection1 = 0;
    assert(classify_j3d_specular_color_material(state, lighting, colorSpecular) ==
           J3dSpecularColorResult::UnsupportedColorProgram);
    state.konstAlphaSelection1 = 0x1C;
    state.hasVertexColor = false;
    assert(classify_j3d_specular_color_material(state, lighting, colorSpecular) ==
           J3dSpecularColorResult::MissingVertexColor);

    ModelLightingContext invalidLighting = lighting;
    invalidLighting.specular.shininess = 0;
    state = material_state();
    assert(classify_j3d_specular_textured_material(state, texture, invalidLighting, material) ==
           J3dSpecularTexturedResult::MissingLightingContext);
}
