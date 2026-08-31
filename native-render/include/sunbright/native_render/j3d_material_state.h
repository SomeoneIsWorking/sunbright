#pragma once

#include <array>
#include <cstdint>

namespace sb::native_render {

// Runtime adapters normalize their different J3D object layouts into this shared state. Material
// classifiers accept only exact semantic families; unsupported programs remain on the reference
// renderer instead of being approximated.
struct J3dMaterialState {
    bool supportedColorBlock = false;
    bool usesMaterialAmbient = false;
    std::uint8_t cullMode = 0xFF;
    bool lightingEnabled = false;
    std::uint8_t colorChannelCount = 0;
    std::uint16_t colorChannelControl = 0;
    std::uint16_t alphaChannelControl = 0;
    std::uint32_t materialColorRgba8 = 0;
    std::uint32_t ambientColorRgba8 = 0;
    std::uint32_t textureCoordinateCount = 0;
    std::uint32_t tevBlockType = 0;
    bool supportedTevBlock = false;
    std::uint8_t tevStageCount = 0;
    std::uint16_t textureNumber0 = 0;
    std::uint8_t textureCoordinate0 = 0;
    std::uint8_t textureMap0 = 0;
    std::uint8_t colorChannel0 = 0;
    std::array<std::uint8_t, 8> tevStage0{};
    std::uint16_t textureNumber1 = 0;
    std::uint8_t textureCoordinate1 = 0;
    std::uint8_t textureMap1 = 0;
    std::uint8_t colorChannel1 = 0;
    std::array<std::uint8_t, 8> tevStage1{};
    std::array<std::uint32_t, 4> konstColorRgba8{};
    std::uint8_t konstColorSelection0 = 0;
    std::uint8_t konstColorSelection1 = 0;
    std::uint8_t konstAlphaSelection0 = 0;
    std::uint8_t konstAlphaSelection1 = 0;
    std::uint32_t pixelEngineBlockType = 0;
    bool hasExplicitPixelPolicy = false;
    std::uint8_t alphaCompare0 = 0;
    std::uint8_t alphaReference0 = 0;
    std::uint8_t alphaOperation = 0;
    std::uint8_t alphaCompare1 = 0;
    std::uint8_t alphaReference1 = 0;
    std::uint8_t blendMode = 0;
    std::uint8_t blendSourceFactor = 0;
    std::uint8_t blendDestinationFactor = 0;
    std::uint8_t blendLogicOperation = 0;
    bool depthTest = false;
    std::uint8_t depthCompare = 0;
    bool depthWrite = false;
    bool fogEnabled = false;
    bool hasVertexColor = false;
    bool hasNormal = false;
};

} // namespace sb::native_render
