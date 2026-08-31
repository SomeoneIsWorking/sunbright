#include <sunbright/native_render/j3d_alpha_masked_material.h>

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
        .cullMode = static_cast<std::uint8_t>(ModelCullMode::Back),
        .lightingEnabled = true,
        .colorChannelCount = 1,
        .colorChannelControl = 0x070E,
        .alphaChannelControl = 0x0700,
        .textureCoordinateCount = 1,
        .tevBlockType = 0x54564232U,
        .supportedTevBlock = true,
        .tevStageCount = 1,
        .textureBindings = {j3d_texture_binding(0)},
        .tevStages = {j3d_tev_stage(0, 0, 4, {0xC0, 0x08, 0xF2, 0xAF, 0xC1, 0x28, 0xF0, 0xF0})},
        .hasTevColors = true,
        .tevColorsS10 = {{{0, 0, 0, 255}}},
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
    const PictureTexture texture{.resource = 5, .width = 32, .height = 32};
    AlphaMaskedColorMaterial material{};
    assert(classify_j3d_alpha_masked_material(state, texture, material) ==
           J3dAlphaMaskedMaterialResult::Success);
    assert(material.color == (Color{0.0F, 0.0F, 0.0F, 1.0F}));
    assert(near(material.alphaScale, 4.0F));
    assert(material.raster.alphaTest == ModelAlphaTest::GreaterOrEqualHalf);
    assert(material.raster.cull == ModelCullMode::Back);

    ModelDraw draw{.instance = 1,
                   .mesh = {.resource = 1, .revision = 1, .vertexCount = 3},
                   .pose = {.modelViews = {sb::native_render::Matrix3x4{
                                .value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}},
                            .count = 1},
                   .projection = {.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}},
                   .material = material};
    const ClipVertex transformed = transform_vertex(draw, MeshVertex{});
    assert(transformed.color == (Color{0.0F, 0.0F, 0.0F, 4.0F}));
    assert(transformed.additiveColor == Color{});

    state.tevColorsS10[0][0] = 1;
    assert(classify_j3d_alpha_masked_material(state, texture, material) ==
           J3dAlphaMaskedMaterialResult::UnsupportedRegisterColor);
    state = material_state();
    state.hasTevColors = false;
    assert(classify_j3d_alpha_masked_material(state, texture, material) ==
           J3dAlphaMaskedMaterialResult::UnsupportedRegisterColor);
    state = material_state();
    state.tevStages[0].program[5] ^= 0x10;
    assert(classify_j3d_alpha_masked_material(state, texture, material) ==
           J3dAlphaMaskedMaterialResult::UnsupportedColorProgram);
    state = material_state();
    state.hasNormal = false;
    assert(classify_j3d_alpha_masked_material(state, texture, material) ==
           J3dAlphaMaskedMaterialResult::MissingNormal);
    state = material_state();
    state.alphaReference0 = 127;
    assert(classify_j3d_alpha_masked_material(state, texture, material) ==
           J3dAlphaMaskedMaterialResult::UnsupportedRasterPolicy);
}
