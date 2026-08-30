#include <sunbright/native_render/j3d_unlit_material.h>

#include <cassert>

int main() {
    using namespace sb::native_render;
    J3dUnlitMaterialState state{
        .supportedColorBlock = true,
        .lightingEnabled = false,
        .colorChannelCount = 1,
        .colorChannelControl = 0,
        .materialColorRgba8 = 0x804020FFU,
        .textureCoordinateCount = 0,
        .tevBlockType = 0x54564231U,
        .supportedTevBlock = true,
        .tevStageCount = 1,
        .textureNumber0 = 0xFFFFU,
        .textureCoordinate0 = 0xFFU,
        .textureMap0 = 0xFFU,
        .colorChannel0 = 4,
        .tevStage0 = {0xC0, 0x40, 0xAF, 0xF0, 0xC1, 0x08, 0xBF, 0x80},
        .hasVertexColor = false,
    };
    UnlitColorMaterial material{};
    assert(classify_j3d_unlit_material(state, material) == J3dUnlitMaterialResult::Success);
    assert(!material.usesVertexColor);
    assert(material.baseColor == Color(128.0F / 255.0F, 64.0F / 255.0F, 32.0F / 255.0F, 1.0F));

    state.textureCoordinateCount = 3;
    assert(classify_j3d_unlit_material(state, material) == J3dUnlitMaterialResult::Success);

    state.colorChannelControl = 1;
    assert(classify_j3d_unlit_material(state, material) ==
           J3dUnlitMaterialResult::MissingVertexColor);
    state.hasVertexColor = true;
    assert(classify_j3d_unlit_material(state, material) == J3dUnlitMaterialResult::Success);
    assert(material.usesVertexColor);
    assert(material.baseColor == Color(1, 1, 1, 1));

    state.tevStage0[2] = 0;
    assert(classify_j3d_unlit_material(state, material) ==
           J3dUnlitMaterialResult::UnsupportedColorProgram);
    state.tevStage0[2] = 0xAF;
    state.textureNumber0 = 0;
    const J3dUnlitMaterialFeatures textured = inspect_j3d_unlit_material(state);
    assert(textured.textureBound);
    assert(!textured.lightingEnabled);
    assert(textured.singleTevStage);
    assert(textured.rasterColorPassThrough);
    assert(classify_j3d_unlit_material(state, material) == J3dUnlitMaterialResult::TextureBinding);

    state.textureCoordinateCount = 1;
    state.textureCoordinate0 = 0;
    state.textureMap0 = 0;
    state.tevStage0 = {0xC0, 0x08, 0xF8, 0xAF, 0xC1, 0x08, 0xF2, 0xF0};
    PictureTexture texture{.resource = 7, .width = 16, .height = 8};
    UnlitTexturedMaterial texturedMaterial{};
    assert(classify_j3d_unlit_textured_material(state, texture, texturedMaterial) ==
           J3dUnlitTexturedResult::Success);
    assert(texturedMaterial.texture == texture);
    assert(texturedMaterial.usesVertexColor);

    state.tevStage0[2] ^= 1U;
    assert(classify_j3d_unlit_textured_material(state, texture, texturedMaterial) ==
           J3dUnlitTexturedResult::UnsupportedColorProgram);
}
