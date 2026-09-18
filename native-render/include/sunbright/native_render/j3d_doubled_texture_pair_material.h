#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/native_render/model.h>

namespace sb::native_render {

enum class J3dDoubledTexturePairResult : std::uint8_t {
    Success,
    UnsupportedColorBlock,
    UnsupportedColorChannels,
    UnsupportedTevBlock,
    UnsupportedStageCount,
    MissingTextureCoordinate,
    UnsupportedTextureBinding,
    UnsupportedColorProgram,
    MissingRegisterColor,
    MissingVertexColor,
    UnsupportedRasterPolicy,
};

[[nodiscard]] const char*
j3d_doubled_texture_pair_result_name(J3dDoubledTexturePairResult result) noexcept;

// Two textures multiplied together and doubled, tinted by one authored colour: the console idiom
// for a surface carrying a baked light map. GMSE01 authors it two ways -- the tint from the channel
// the first stage rasterises, or from a colour constant the raster never reaches -- and the second
// spelling leaves a lit channel enabled whose output no stage reads. Both are this one material,
// so the rule accepts a lit channel only when it can see that nothing consumes it.
[[nodiscard]] J3dDoubledTexturePairResult
classify_j3d_doubled_texture_pair_material(const J3dMaterialState& state,
                                           const PictureTexture& base, const PictureTexture& detail,
                                           DoubledTexturePairMaterial& material) noexcept;

} // namespace sb::native_render
