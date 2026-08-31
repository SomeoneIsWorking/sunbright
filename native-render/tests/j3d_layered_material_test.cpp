#include <sunbright/native_render/j3d_layered_material.h>

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
        .cullMode = static_cast<std::uint8_t>(ModelCullMode::None),
        .lightingEnabled = true,
        .colorChannelCount = 1,
        .colorChannelControl = 0x0686,
        .alphaChannelControl = 0x0706,
        .materialColorRgba8 = 0x80808080,
        .ambientColorRgba8 = 0x404040FF,
        .textureCoordinateCount = 2,
        .tevBlockType = 0x54564232U,
        .supportedTevBlock = true,
        .tevStageCount = 2,
        .textureBindings = {j3d_texture_binding(1), j3d_texture_binding(2)},
        .tevStages = {j3d_tev_stage(1, 1, 4, {0xC0, 0x08, 0x8A, 0xEF, 0xC1, 0x08, 0xFF, 0xD0},
                                    0x03),
                      j3d_tev_stage(0, 0, 0xFF, {0xC2, 0x08, 0xF0, 0x8F, 0xC3, 0x08, 0xF0, 0x70})},
        .pixelEngineBlockType = 0x5045464CU,
        .hasExplicitPixelPolicy = true,
        .alphaCompare0 = 6,
        .alphaReference0 = 128,
        .alphaOperation = 0,
        .alphaCompare1 = 3,
        .alphaReference1 = 255,
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
    const PictureTexture baseTexture{.resource = 10, .width = 32, .height = 32};
    const PictureTexture detailTexture{.resource = 11, .width = 16, .height = 16};
    const ModelLightingContext lighting{
        .pointLights =
            {{{.position = {0, 0, 1}, .color = {1, 1, 1, 1}, .distanceAttenuation = {1, 0, 0}}}},
        .pointLightCount = 1,
    };
    LitLayeredTexturedMaterial material{};
    assert(classify_j3d_layered_material(state, baseTexture, detailTexture, lighting, material) ==
           J3dLayeredMaterialResult::Success);
    assert(material.baseTexture == baseTexture);
    assert(material.detailTexture == detailTexture);
    assert(near(material.detailWeight, 3.0F / 8.0F));
    assert(material.diffuseMode == ModelDiffuseMode::Signed);

    const ModelDraw draw{
        .instance = 1,
        .mesh = {.resource = 2, .revision = 1, .vertexCount = 3},
        .pose = {.modelViews = {Matrix3x4{.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}},
                 .count = 1},
        .projection = {.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}},
        .material = material,
    };
    const MeshVertex backFacing{.uv = {0.2F, 0.3F}, .uv1 = {0.7F, 0.8F}, .normal = {0, 0, -1}};
    const ClipVertex transformed = transform_vertex(draw, backFacing);
    // Signed diffuse subtracts the primary light from the 0.25 ambient floor, then clamps to zero.
    assert(near(transformed.color.r, 0.0F));
    assert(near(transformed.color.a, 0x80 / 255.0F));
    assert(near(transformed.detailTextureWeight, 3.0F / 8.0F));

    state.tevStages[0].program[2] ^= 1U;
    assert(classify_j3d_layered_material(state, baseTexture, detailTexture, lighting, material) ==
           J3dLayeredMaterialResult::UnsupportedColorProgram);
    state = material_state();
    state.textureCoordinateCount = 1;
    assert(classify_j3d_layered_material(state, baseTexture, detailTexture, lighting, material) ==
           J3dLayeredMaterialResult::MissingTextureCoordinates);
}
