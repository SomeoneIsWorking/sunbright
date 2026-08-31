#pragma once

#include <sunbright/native_render/j3d_fog.h>

#include <array>
#include <compare>
#include <cstdint>

namespace sb::native_render {

constexpr std::size_t kMaxJ3dTextureMaps = 8;
constexpr std::size_t kMaxJ3dTevStages = 16;
constexpr std::size_t kJ3dTevColorRegisters = 3;

// A texture-map slot is independent of the number of active colour stages. A stage refers to a
// map by index; the runtime adapters resolve that map to this source texture-table slot.
struct J3dTextureBinding {
    std::uint16_t textureNumber = 0xFFFF;

    auto operator<=>(const J3dTextureBinding&) const = default;
};

// One high-level J3D colour stage as copied from each runtime's object layout. The shared
// renderer classifies the complete bounded program and never receives these console encodings.
struct J3dTevStageState {
    std::uint8_t textureCoordinate = 0;
    std::uint8_t textureMap = 0;
    std::uint8_t colorChannel = 0;
    std::array<std::uint8_t, 8> program{};
    std::uint8_t konstColorSelection = 0;
    std::uint8_t konstAlphaSelection = 0;

    auto operator<=>(const J3dTevStageState&) const = default;
};

[[nodiscard]] constexpr J3dTextureBinding
j3d_texture_binding(std::uint16_t textureNumber) noexcept {
    return {.textureNumber = textureNumber};
}

[[nodiscard]] constexpr J3dTevStageState
j3d_tev_stage(std::uint8_t textureCoordinate, std::uint8_t textureMap, std::uint8_t colorChannel,
              std::array<std::uint8_t, 8> program, std::uint8_t konstColorSelection = 0,
              std::uint8_t konstAlphaSelection = 0) noexcept {
    return {.textureCoordinate = textureCoordinate,
            .textureMap = textureMap,
            .colorChannel = colorChannel,
            .program = program,
            .konstColorSelection = konstColorSelection,
            .konstAlphaSelection = konstAlphaSelection};
}

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
    std::uint16_t colorChannelControl1 = 0;
    std::uint16_t alphaChannelControl1 = 0;
    std::uint32_t materialColorRgba8 = 0;
    std::uint32_t ambientColorRgba8 = 0;
    std::uint32_t materialColor1Rgba8 = 0;
    std::uint32_t ambientColor1Rgba8 = 0;
    std::uint32_t textureCoordinateCount = 0;
    std::uint32_t tevBlockType = 0;
    bool supportedTevBlock = false;
    std::uint8_t tevStageCount = 0;
    std::array<J3dTextureBinding, kMaxJ3dTextureMaps> textureBindings{};
    std::array<J3dTevStageState, kMaxJ3dTevStages> tevStages{};
    bool hasTevColors = false;
    std::array<std::array<std::int16_t, 4>, kJ3dTevColorRegisters> tevColorsS10{};
    std::array<std::uint32_t, 4> konstColorRgba8{};
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
    J3dFogState fog{};
    bool hasVertexColor = false;
    bool hasNormal = false;
};

// J3D's texture binding slots are independent of its active colour-stage count. A stage order
// selects one slot by texture-map index; unsupported slots remain explicitly unbound.
[[nodiscard]] constexpr std::uint16_t j3d_texture_number_for_map(const J3dMaterialState& state,
                                                                 std::uint8_t textureMap) noexcept {
    return textureMap < state.textureBindings.size()
               ? state.textureBindings[textureMap].textureNumber
               : 0xFFFF;
}

} // namespace sb::native_render
