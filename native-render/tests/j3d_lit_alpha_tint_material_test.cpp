#include <sunbright/native_render/j3d_lit_alpha_tint_material.h>

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
        .colorChannelControl = 0x0706,
        .alphaChannelControl = 0x0700,
        .materialColorRgba8 = 0x80402080,
        .ambientColorRgba8 = 0x808080FF,
        .textureCoordinateCount = 1,
        .tevBlockType = 0x54564232U,
        .supportedTevBlock = true,
        .tevStageCount = 1,
        .textureBindings = {j3d_texture_binding(3)},
        .tevStages = {j3d_tev_stage(0, 0, 4, {0xC0, 0x08, 0xF8, 0x2F, 0xC1, 0x08, 0xF2, 0xF0})},
        .hasTevColors = true,
        .tevColorsS10 = {{{64, 128, 192, 255}}},
        .pixelEngineBlockType = 0x5045464CU,
        .hasExplicitPixelPolicy = true,
        .alphaCompare0 = 7,
        .alphaOperation = 0,
        .alphaCompare1 = 7,
        .blendMode = 0,
        .blendSourceFactor = 1,
        .blendDestinationFactor = 0,
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
        .pointLights = {{{.position = {0, 0, 10}, .color = {0, 0, 0, 1}}}},
        .pointLightCount = 1,
        .ambientColor = {0.5F, 0.5F, 0.5F, 1.0F},
    };
    LitAlphaTintMaterial material{};
    assert(classify_j3d_lit_alpha_tint_material(state, texture, lighting, material) ==
           J3dLitAlphaTintResult::Success);
    assert(material.texture == texture);
    assert(near(material.tint.r, 64.0F / 255.0F));
    assert(near(material.tint.g, 128.0F / 255.0F));
    assert(near(material.tint.b, 192.0F / 255.0F));
    assert(material.baseColor == color_from_rgba8(state.materialColorRgba8));

    ModelDraw draw{
        .instance = 1,
        .mesh = {.resource = 1, .revision = 1, .vertexCount = 3},
        .pose = {.modelViews = {Matrix3x4{.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}},
                 .count = 1},
        .projection = {.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}},
        .material = material};
    const ClipVertex transformed = transform_vertex(draw, MeshVertex{});
    const Color base = color_from_rgba8(state.materialColorRgba8);
    const Color ambient = color_from_rgba8(state.ambientColorRgba8);
    assert(near(transformed.color.r, base.r * ambient.r * 64.0F / 255.0F));
    assert(near(transformed.color.g, base.g * ambient.g * 128.0F / 255.0F));
    assert(near(transformed.color.b, base.b * ambient.b * 192.0F / 255.0F));
    assert(near(transformed.color.a, 128.0F / 255.0F));
    assert(material_texture_count(draw.material) == 1);
    assert(material_texture(draw.material) ==
           &std::get<LitAlphaTintMaterial>(draw.material).texture);

    state.tevStages[0].program[2] ^= 1U;
    assert(classify_j3d_lit_alpha_tint_material(state, texture, lighting, material) ==
           J3dLitAlphaTintResult::UnsupportedColorProgram);
    state = material_state();
    state.tevColorsS10[0][0] = 63;
    assert(classify_j3d_lit_alpha_tint_material(state, texture, lighting, material) ==
           J3dLitAlphaTintResult::Success);
    state = material_state();
    state.hasNormal = false;
    assert(classify_j3d_lit_alpha_tint_material(state, texture, lighting, material) ==
           J3dLitAlphaTintResult::MissingNormal);
    state = material_state();
    const ModelLightingContext noLights{};
    assert(classify_j3d_lit_alpha_tint_material(state, texture, noLights, material) ==
           J3dLitAlphaTintResult::MissingLightingContext);
    state = material_state();
    state.alphaCompare0 = 6;
    state.alphaReference0 = 1;
    assert(classify_j3d_lit_alpha_tint_material(state, texture, lighting, material) ==
           J3dLitAlphaTintResult::UnsupportedRasterPolicy);
}
