#include <sunbright/native_render/j3d_dual_alpha_effect_material.h>

#include <cassert>
#include <cmath>

namespace {

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001F;
}

sb::native_render::J3dMaterialState material_state() {
    using namespace sb::native_render;
    J3dMaterialState state{
        .supportedColorBlock = true,
        .cullMode = static_cast<std::uint8_t>(ModelCullMode::Back),
        .lightingEnabled = true,
        .colorChannelCount = 1,
        .colorChannelControl = 0x0706,
        .alphaChannelControl = 0x0700,
        .materialColorRgba8 = 0x804020FF,
        .textureCoordinateCount = 2,
        .supportedTevBlock = true,
        .tevStageCount = 3,
        .textureBindings = {j3d_texture_binding(2), j3d_texture_binding(3)},
        .tevStages =
            {
                j3d_tev_stage(0, 0, 4, {0xC0, 0x08, 0xFF, 0xF2, 0xC1, 0x08, 0xFF, 0xC0}),
                j3d_tev_stage(1, 1, 4, {0xC2, 0x18, 0xF4, 0x0A, 0xC3, 0x10, 0xF0, 0x50}),
                j3d_tev_stage(0xFF, 0xFF, 4, {0xC4, 0x00, 0xFF, 0xF0, 0xC5, 0x00, 0xF4, 0x70}),
            },
        .hasTevColors = true,
        .tevColorsS10 = {{{80, 120, 160, 200}, {40, 50, 60, 70}, {0, 0, 0, 0}}},
        .pixelEngineBlockType = 0x5045464CU,
        .hasExplicitPixelPolicy = true,
        .alphaCompare0 = 7,
        .alphaCompare1 = 7,
        .blendMode = 1,
        .blendSourceFactor = 4,
        .blendDestinationFactor = 2,
        .depthTest = true,
        .depthCompare = 3,
        .depthWrite = false,
        .hasNormal = true,
    };
    return state;
}

sb::native_render::ModelLightingContext lighting() {
    using namespace sb::native_render;
    ModelLightingContext result{};
    result.pointLightCount = 1;
    result.pointLights[0].position = {0.0F, 0.0F, 4.0F};
    result.pointLights[0].distanceAttenuation = {1.0F, 0.0F, 0.0F};
    return result;
}

} // namespace

int main() {
    using namespace sb::native_render;
    const PictureTexture first{.resource = 9, .width = 64, .height = 64};
    const PictureTexture second{.resource = 10, .width = 64, .height = 64};
    LitDualAlphaEffectMaterial material{};
    const J3dMaterialState state = material_state();
    assert(is_j3d_dual_alpha_effect_material(state));
    assert(classify_j3d_dual_alpha_effect_material(state, first, second, lighting(), material) ==
           J3dDualAlphaEffectMaterialResult::Success);
    assert(material.firstTexture == first);
    assert(material.secondTexture == second);
    assert(near(material.baseColor.r, 128.0F / 255.0F));
    assert(near(material.baseColor.g, 64.0F / 255.0F));
    assert(near(material.baseColor.b, 32.0F / 255.0F));
    assert(near(material.additiveColor.r, 2.0F * (80.0F / 255.0F) * (40.0F / 255.0F)));
    assert(near(material.additiveColor.g, 2.0F * (120.0F / 255.0F) * (50.0F / 255.0F)));
    assert(near(material.additiveColor.b, 2.0F * (160.0F / 255.0F) * (60.0F / 255.0F)));
    assert(material.lighting.pointLightCount == 1);
    assert(material.raster.blend == ModelBlendMode::SourceAlphaSourceColor);

    J3dMaterialState changed = state;
    changed.tevStages[1].program[2] ^= 1U;
    assert(!is_j3d_dual_alpha_effect_material(changed));
    assert(classify_j3d_dual_alpha_effect_material(changed, first, second, lighting(), material) ==
           J3dDualAlphaEffectMaterialResult::UnsupportedStageCount);
}
