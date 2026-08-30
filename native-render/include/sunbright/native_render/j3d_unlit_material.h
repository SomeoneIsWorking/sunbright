#pragma once

#include <sunbright/native_render/model.h>

#include <array>
#include <cstdint>

namespace sb::native_render {

// Runtime adapters normalize their different J3D object layouts into this state. The classifier
// below is the sole definition of the first supported material family.
struct J3dUnlitMaterialState {
    bool supportedColorBlock = false;
    bool lightingEnabled = false;
    std::uint8_t colorChannelCount = 0;
    std::uint16_t colorChannelControl = 0;
    std::uint32_t materialColorRgba8 = 0;
    std::uint32_t textureCoordinateCount = 0;
    std::uint32_t tevBlockType = 0;
    bool supportedTevBlock = false;
    std::uint8_t tevStageCount = 0;
    std::uint16_t textureNumber0 = 0;
    std::uint8_t textureCoordinate0 = 0;
    std::uint8_t textureMap0 = 0;
    std::uint8_t colorChannel0 = 0;
    std::array<std::uint8_t, 8> tevStage0{};
    bool hasVertexColor = false;
};

enum class J3dUnlitMaterialResult : std::uint8_t {
    Success,
    UnsupportedColorBlock,
    Lighting,
    MissingColorChannel,
    TextureBinding,
    UnsupportedTevBlock,
    MultipleTevStages,
    UnsupportedColorProgram,
    MissingVertexColor,
};

enum class J3dUnlitTexturedResult : std::uint8_t {
    Success,
    UnsupportedColorBlock,
    Lighting,
    MissingColorChannel,
    UnsupportedTevBlock,
    MultipleTevStages,
    MissingTextureCoordinate,
    UnsupportedTextureBinding,
    UnsupportedColorProgram,
    MissingVertexColor,
};

struct J3dUnlitMaterialFeatures {
    bool supportedColorBlock = false;
    bool lightingEnabled = false;
    bool hasColorChannel = false;
    bool textureBound = false;
    bool supportedTevBlock = false;
    bool singleTevStage = false;
    bool rasterColorPassThrough = false;
    bool requiredVertexColorPresent = false;
};

[[nodiscard]] const char* j3d_unlit_material_result_name(J3dUnlitMaterialResult result) noexcept;
[[nodiscard]] J3dUnlitMaterialFeatures
inspect_j3d_unlit_material(const J3dUnlitMaterialState& state) noexcept;
[[nodiscard]] J3dUnlitMaterialResult
classify_j3d_unlit_material(const J3dUnlitMaterialState& state,
                            UnlitColorMaterial& material) noexcept;
[[nodiscard]] const char* j3d_unlit_textured_result_name(J3dUnlitTexturedResult result) noexcept;
[[nodiscard]] J3dUnlitTexturedResult
classify_j3d_unlit_textured_material(const J3dUnlitMaterialState& state,
                                     const PictureTexture& texture,
                                     UnlitTexturedMaterial& material) noexcept;

} // namespace sb::native_render
