#include <sunbright/native_render/j3d_lit_material.h>
#include <sunbright/native_render/j3d_stage_lighting.h>

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
        .colorChannelCount = 1,
        .colorChannelControl = 0x070E,
        .alphaChannelControl = 0x0700,
        .materialColorRgba8 = 0x804020FF,
        .ambientColorRgba8 = 0x102030FF,
        .textureCoordinateCount = 1,
        .tevBlockType = 0x54564231U,
        .supportedTevBlock = true,
        .tevStageCount = 1,
        .textureNumber0 = 3,
        .textureCoordinate0 = 0,
        .textureMap0 = 0,
        .colorChannel0 = 4,
        .tevStage0 = {0xC0, 0x08, 0xF8, 0xAF, 0xC1, 0x08, 0xF2, 0xF0},
        .pixelEngineBlockType = 0x50454F50U,
        .hasNormal = true,
    };
}

} // namespace

int main() {
    using namespace sb::native_render;

    const J3dStageLightingInput input{
        .view = {.value = {1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30}},
        .primaryWorldPosition = {1, 2, 3},
        .primaryColor = {1, 0.5F, 0.25F, 1},
        .shininess = 50.0F,
        .ambientColor = {0.2F, 0.3F, 0.4F, 1},
        .effectEnabled = true,
        .effectWorldPosition = {4, 5, 6},
        .effectColor = {0.25F, 0.5F, 1, 1},
    };
    const ModelLightingContext lighting = build_j3d_stage_lighting(input);
    assert(lighting.pointLightCount == 2);
    assert(lighting.ambientColor == input.ambientColor);
    assert((lighting.pointLights[0].position == Vec3{11, 22, 33}));
    assert((lighting.pointLights[0].distanceAttenuation == Vec3{1, 0, 0}));
    assert((lighting.pointLights[1].position == Vec3{14, 25, 36}));
    assert(near(lighting.pointLights[1].distanceAttenuation.y, 0.0005F));
    assert(near(lighting.pointLights[1].distanceAttenuation.z, 0.0000005F));
    assert(near(lighting.specular.directionToLight.x,
                11.0F / std::sqrt(11.0F * 11.0F + 22.0F * 22.0F + 33.0F * 33.0F)));
    assert(lighting.specular.color == input.primaryColor);
    assert(near(lighting.specular.shininess, input.shininess));
    clear_j3d_stage_lighting();
    assert(current_j3d_stage_lighting() == nullptr);
    publish_j3d_stage_lighting(input);
    assert(current_j3d_stage_lighting() != nullptr);
    assert(*current_j3d_stage_lighting() == lighting);

    const PictureTexture texture{.resource = 5, .width = 32, .height = 32};
    J3dMaterialState state = material_state();
    LitTexturedMaterial material{};
    assert(classify_j3d_lit_textured_material(state, texture, lighting, material) ==
           J3dLitTexturedResult::Success);
    assert(!material.usesVertexRgb);
    assert(!material.usesVertexAlpha);
    assert(near(material.litColorWeight, 1.0F));
    assert(material.baseColor == color_from_rgba8(0x804020FF));
    assert(material.ambientColor == color_from_rgba8(0x102030FF));
    assert(material.lighting == lighting);

    state.usesMaterialAmbient = false;
    assert(classify_j3d_lit_textured_material(state, texture, lighting, material) ==
           J3dLitTexturedResult::Success);
    assert(material.ambientColor == lighting.ambientColor);
    state.usesMaterialAmbient = true;

    state.colorChannelControl = 0x070F;
    state.alphaChannelControl = 0x0701;
    assert(classify_j3d_lit_textured_material(state, texture, lighting, material) ==
           J3dLitTexturedResult::MissingVertexColor);
    state.hasVertexColor = true;
    assert(classify_j3d_lit_textured_material(state, texture, lighting, material) ==
           J3dLitTexturedResult::Success);
    assert(material.usesVertexRgb);
    assert(material.usesVertexAlpha);

    state.hasNormal = false;
    assert(classify_j3d_lit_textured_material(state, texture, lighting, material) ==
           J3dLitTexturedResult::MissingNormal);
    state.hasNormal = true;
    state.tevStageCount = 2;
    assert(classify_j3d_lit_textured_material(state, texture, lighting, material) ==
           J3dLitTexturedResult::UnsupportedStageCount);

    // Sunshine's hand material expresses a semantic 50/50 mix between white and the diffuse-lit
    // vertex colour, followed by one ordinary texture. The classifier translates that authored
    // meaning rather than exposing the two console colour stages to the renderer.
    state.colorChannelControl = 0x0707;
    state.alphaChannelControl = 0x0700;
    state.textureCoordinate0 = 0xFF;
    state.textureMap0 = 0xFF;
    state.colorChannel0 = 4;
    state.tevStage0 = {0xC0, 0x08, 0xCA, 0xEF, 0xC1, 0x08, 0xFF, 0xD0};
    state.textureNumber1 = 0xFFFF;
    state.textureCoordinate1 = 0;
    state.textureMap1 = 0;
    state.colorChannel1 = 0xFF;
    state.tevStage1 = {0xC2, 0x08, 0xF0, 0x8F, 0xC3, 0x08, 0xF0, 0x70};
    state.konstColorSelection0 = 0x04;
    state.pixelEngineBlockType = 0x5045464C;
    state.hasExplicitPixelPolicy = true;
    state.alphaCompare0 = 7;
    state.alphaReference0 = 0;
    state.alphaOperation = 1;
    state.alphaCompare1 = 7;
    state.alphaReference1 = 0;
    state.blendMode = 1;
    state.blendSourceFactor = 4;
    state.blendDestinationFactor = 5;
    state.blendLogicOperation = 3;
    state.depthTest = true;
    state.depthCompare = 3;
    state.depthWrite = false;
    assert(classify_j3d_lit_textured_material(state, texture, lighting, material) ==
           J3dLitTexturedResult::Success);
    assert(material.usesVertexRgb);
    assert(!material.usesVertexAlpha);
    assert(near(material.litColorWeight, 0.5F));
    assert(material.raster.blend == ModelBlendMode::SourceAlpha);
    assert(!material.raster.depthWrite);
    state.konstColorSelection0 = 0x03;
    assert(classify_j3d_lit_textured_material(state, texture, lighting, material) ==
           J3dLitTexturedResult::UnsupportedColorProgram);
    state.konstColorSelection0 = 0x04;
    state.alphaOperation = 2;
    assert(classify_j3d_lit_textured_material(state, texture, lighting, material) ==
           J3dLitTexturedResult::UnsupportedRasterPolicy);
}
