#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/native_render/model.h>

#include <cstdint>

namespace sb::native_render {

enum class J3dRasterPolicyResult : std::uint8_t {
    Success,
    UnsupportedCullMode,
    UnsupportedPixelEngineBlock,
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
    UnsupportedRasterPolicy,
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
    UnsupportedRasterPolicy,
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
[[nodiscard]] J3dRasterPolicyResult classify_j3d_raster_policy(const J3dMaterialState& state,
                                                               ModelRasterPolicy& policy) noexcept;
[[nodiscard]] J3dUnlitMaterialFeatures
inspect_j3d_unlit_material(const J3dMaterialState& state) noexcept;
[[nodiscard]] J3dUnlitMaterialResult
classify_j3d_unlit_material(const J3dMaterialState& state, UnlitColorMaterial& material) noexcept;
[[nodiscard]] const char* j3d_unlit_textured_result_name(J3dUnlitTexturedResult result) noexcept;
[[nodiscard]] J3dUnlitTexturedResult
classify_j3d_unlit_textured_material(const J3dMaterialState& state, const PictureTexture& texture,
                                     UnlitTexturedMaterial& material) noexcept;

} // namespace sb::native_render
