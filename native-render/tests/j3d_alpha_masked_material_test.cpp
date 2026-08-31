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
        .textureNumber0 = 0,
        .textureCoordinate0 = 0,
        .textureMap0 = 0,
        .colorChannel0 = 4,
        .tevStage0 = {0xC0, 0x08, 0xF2, 0xAF, 0xC1, 0x28, 0xF0, 0xF0},
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
                   .modelView = {.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}},
                   .projection = {.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}},
                   .material = material};
    const ClipVertex transformed = transform_vertex(draw, MeshVertex{});
    assert(transformed.color == (Color{0.0F, 0.0F, 0.0F, 4.0F}));
    assert(transformed.additiveColor == Color{});

    state.tevColor0S10[0] = 1;
    assert(classify_j3d_alpha_masked_material(state, texture, material) ==
           J3dAlphaMaskedMaterialResult::UnsupportedRegisterColor);
    state = material_state();
    state.hasTevColor0 = false;
    assert(classify_j3d_alpha_masked_material(state, texture, material) ==
           J3dAlphaMaskedMaterialResult::UnsupportedRegisterColor);
    state = material_state();
    state.tevStage0[5] ^= 0x10;
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
