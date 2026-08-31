#include <sunbright/native_render/j3d_unlit_material.h>

#include <cassert>

int main() {
    using namespace sb::native_render;
    J3dMaterialState state{
        .supportedColorBlock = true,
        .cullMode = 2,
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
        .pixelEngineBlockType = 0x50454F50U,
        .hasVertexColor = false,
    };
    UnlitColorMaterial material{};
    assert(classify_j3d_unlit_material(state, material) == J3dUnlitMaterialResult::Success);
    assert(!material.usesVertexColor);
    assert(material.raster.cull == ModelCullMode::Back);
    assert(material.raster.depthWrite);
    assert(material.raster.alphaTest == ModelAlphaTest::PassAll);
    assert(material.raster.blend == ModelBlendMode::Replace);
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
    assert(texturedMaterial.textureCoordinates == ModelTextureCoordinates::Primary);
    assert(texturedMaterial.texture == texture);
    assert(texturedMaterial.usesVertexColor);

    state.textureNumber0 = 0xFFFF;
    state.textureNumber1 = 3;
    state.textureCoordinateCount = 2;
    state.textureCoordinate0 = 1;
    state.textureMap0 = 1;
    assert(classify_j3d_unlit_textured_material(state, texture, texturedMaterial) ==
           J3dUnlitTexturedResult::Success);
    assert(texturedMaterial.textureCoordinates == ModelTextureCoordinates::Secondary);
    state.textureMap0 = 2;
    assert(classify_j3d_unlit_textured_material(state, texture, texturedMaterial) ==
           J3dUnlitTexturedResult::UnsupportedTextureBinding);
    state.textureNumber0 = 0;
    state.textureNumber1 = 0xFFFF;
    state.textureCoordinateCount = 1;
    state.textureCoordinate0 = 0;
    state.textureMap0 = 0;

    state.pixelEngineBlockType = 0x50454544U;
    assert(classify_j3d_unlit_textured_material(state, texture, texturedMaterial) ==
           J3dUnlitTexturedResult::Success);
    assert(texturedMaterial.raster.alphaTest == ModelAlphaTest::GreaterOrEqualHalf);
    assert(texturedMaterial.raster.depthWrite);
    state.pixelEngineBlockType = 0x5045584CU;
    assert(classify_j3d_unlit_textured_material(state, texture, texturedMaterial) ==
           J3dUnlitTexturedResult::Success);
    assert(texturedMaterial.raster.blend == ModelBlendMode::SourceAlpha);
    assert(!texturedMaterial.raster.depthWrite);
    state.pixelEngineBlockType = 0x5045464CU;
    assert(classify_j3d_unlit_textured_material(state, texture, texturedMaterial) ==
           J3dUnlitTexturedResult::UnsupportedRasterPolicy);
    state.hasExplicitPixelPolicy = true;
    state.alphaCompare0 = 7;
    state.alphaReference0 = 0;
    state.alphaOperation = 0;
    state.alphaCompare1 = 7;
    state.alphaReference1 = 0;
    state.blendMode = 0;
    state.blendSourceFactor = 1;
    state.blendDestinationFactor = 0;
    state.blendLogicOperation = 3;
    state.depthTest = true;
    state.depthCompare = 3;
    state.depthWrite = true;
    assert(classify_j3d_unlit_textured_material(state, texture, texturedMaterial) ==
           J3dUnlitTexturedResult::Success);
    assert(texturedMaterial.raster == ModelRasterPolicy{.cull = ModelCullMode::Back});
    state.depthWrite = false;
    assert(classify_j3d_unlit_textured_material(state, texture, texturedMaterial) ==
           J3dUnlitTexturedResult::Success);
    assert(!texturedMaterial.raster.depthWrite);
    assert(texturedMaterial.raster.blend == ModelBlendMode::Replace);
    state.depthWrite = true;
    // Combining two always-true comparisons with AND, OR, or XNOR has the same pass-all meaning.
    // Normalize that meaning instead of requiring one incidental console encoding.
    state.alphaOperation = 1;
    assert(classify_j3d_unlit_textured_material(state, texture, texturedMaterial) ==
           J3dUnlitTexturedResult::Success);
    state.alphaOperation = 2;
    assert(classify_j3d_unlit_textured_material(state, texture, texturedMaterial) ==
           J3dUnlitTexturedResult::UnsupportedRasterPolicy);
    state.alphaOperation = 0;
    state.fog = {.type = 2, .start = 300.0F, .end = 1500.0F, .near = 1.0F, .far = 300000.0F};
    assert(classify_j3d_unlit_textured_material(state, texture, texturedMaterial) ==
           J3dUnlitTexturedResult::Success);
    state.fog.type = 4;
    assert(classify_j3d_unlit_textured_material(state, texture, texturedMaterial) ==
           J3dUnlitTexturedResult::UnsupportedRasterPolicy);
    state.fog.type = 2;
    state.fog.rangeAdjustmentEnabled = true;
    assert(classify_j3d_unlit_textured_material(state, texture, texturedMaterial) ==
           J3dUnlitTexturedResult::UnsupportedRasterPolicy);
    state.fog = {};
    state.alphaCompare0 = 6;
    state.alphaReference0 = 0x80;
    state.alphaCompare1 = 3;
    state.alphaReference1 = 0xFF;
    assert(classify_j3d_unlit_textured_material(state, texture, texturedMaterial) ==
           J3dUnlitTexturedResult::Success);
    assert(texturedMaterial.raster.alphaTest == ModelAlphaTest::GreaterOrEqualHalf);
    state.alphaCompare0 = 7;
    state.alphaReference0 = 0;
    state.alphaCompare1 = 7;
    state.alphaReference1 = 0;
    state.blendMode = 1;
    state.blendSourceFactor = 4;
    state.blendDestinationFactor = 5;
    state.depthWrite = false;
    assert(classify_j3d_unlit_textured_material(state, texture, texturedMaterial) ==
           J3dUnlitTexturedResult::Success);
    assert(texturedMaterial.raster.blend == ModelBlendMode::SourceAlpha);
    assert(!texturedMaterial.raster.depthWrite);
    state.blendSourceFactor = 1;
    assert(classify_j3d_unlit_textured_material(state, texture, texturedMaterial) ==
           J3dUnlitTexturedResult::Success);
    assert(texturedMaterial.raster.blend == ModelBlendMode::PremultipliedAlpha);
    assert(!texturedMaterial.raster.depthWrite);
    state.blendSourceFactor = 4;
    state.pixelEngineBlockType = 0x50454F50U;

    state.tevStage0[2] ^= 1U;
    assert(classify_j3d_unlit_textured_material(state, texture, texturedMaterial) ==
           J3dUnlitTexturedResult::UnsupportedColorProgram);
}
