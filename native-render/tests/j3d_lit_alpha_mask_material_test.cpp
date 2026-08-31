#include <sunbright/native_render/j3d_lit_alpha_mask_material.h>

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
        .textureCoordinateCount = 2,
        .tevBlockType = 0x54564232U,
        .supportedTevBlock = true,
        .tevStageCount = 2,
        .textureBindings = {j3d_texture_binding(3), j3d_texture_binding(4)},
        .tevStages = {j3d_tev_stage(0, 0, 4, {0xC0, 0x08, 0xF8, 0xAF, 0xC1, 0x08, 0xFF, 0xC0}),
                      j3d_tev_stage(1, 1, 0xFF, {0xC2, 0x08, 0xFF, 0xF0, 0xC3, 0x28, 0xF0, 0xF0})},
        .hasTevColor0 = true,
        .tevColor0S10 = {0, 0, 0, 255},
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
    const PictureTexture colorTexture{.resource = 5, .width = 32, .height = 32};
    const PictureTexture alphaMaskTexture{.resource = 6, .width = 16, .height = 16};
    const ModelLightingContext lighting{
        .pointLights = {{{.position = {0, 0, 10}, .color = {0.5F, 0.25F, 0.0F, 1}}}},
        .pointLightCount = 1,
    };
    LitTexturedAlphaMaskMaterial material{};
    assert(classify_j3d_lit_alpha_mask_material(state, colorTexture, alphaMaskTexture, lighting,
                                                material) == J3dLitAlphaMaskResult::Success);
    assert(material.colorTexture == colorTexture);
    assert(material.alphaMaskTexture == alphaMaskTexture);
    assert(material.baseColor == color_from_rgba8(state.materialColorRgba8));
    assert(material.ambientColor == color_from_rgba8(state.ambientColorRgba8));
    assert(near(material.alphaScale, 4.0F));
    assert(material.raster.alphaTest == ModelAlphaTest::GreaterOrEqualHalf);

    ModelDraw draw{.instance = 1,
                   .mesh = {.resource = 1, .revision = 1, .vertexCount = 3},
                   .pose = {.modelViews = {sb::native_render::Matrix3x4{
                                .value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}},
                            .count = 1},
                   .projection = {.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}},
                   .material = material};
    const MeshVertex vertex{.uv = {0.25F, 0.5F}, .uv1 = {0.75F, 0.125F}, .normal = {0, 0, 1}};
    const ClipVertex transformed = transform_vertex(draw, vertex);
    assert(transformed.uv == vertex.uv);
    assert(transformed.uv1 == vertex.uv1);
    assert(near(transformed.color.r, (0x80 / 255.0F) * (0x10 / 255.0F + 0.5F)));
    assert(near(transformed.color.g, (0x40 / 255.0F) * (0x20 / 255.0F + 0.25F)));
    assert(near(transformed.color.b, (0x20 / 255.0F) * (0x30 / 255.0F)));
    assert(near(transformed.color.a, 4.0F));

    state.tevStages[1].program[5] ^= 0x10;
    assert(classify_j3d_lit_alpha_mask_material(state, colorTexture, alphaMaskTexture, lighting,
                                                material) ==
           J3dLitAlphaMaskResult::UnsupportedColorProgram);
    state = material_state();
    state.tevColor0S10[3] = 254;
    assert(classify_j3d_lit_alpha_mask_material(state, colorTexture, alphaMaskTexture, lighting,
                                                material) ==
           J3dLitAlphaMaskResult::UnsupportedRegisterColor);
    state = material_state();
    state.tevStages[1].textureCoordinate = 0;
    assert(classify_j3d_lit_alpha_mask_material(state, colorTexture, alphaMaskTexture, lighting,
                                                material) ==
           J3dLitAlphaMaskResult::UnsupportedTextureBindings);
    state = material_state();
    state.hasNormal = false;
    assert(classify_j3d_lit_alpha_mask_material(state, colorTexture, alphaMaskTexture, lighting,
                                                material) == J3dLitAlphaMaskResult::MissingNormal);
    state = material_state();
    ModelLightingContext noLights{};
    assert(classify_j3d_lit_alpha_mask_material(state, colorTexture, alphaMaskTexture, noLights,
                                                material) ==
           J3dLitAlphaMaskResult::MissingLightingContext);
    state = material_state();
    state.alphaReference0 = 127;
    assert(classify_j3d_lit_alpha_mask_material(state, colorTexture, alphaMaskTexture, lighting,
                                                material) ==
           J3dLitAlphaMaskResult::UnsupportedRasterPolicy);
}
