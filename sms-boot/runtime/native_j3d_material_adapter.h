#pragma once

#include <sunbright/native_render/j3d_layered_material.h>
#include <sunbright/native_render/j3d_lit_alpha_mask_material.h>
#include <sunbright/native_render/j3d_lit_material.h>
#include <sunbright/native_render/j3d_masked_toon_material.h>
#include <sunbright/native_render/j3d_material_family.h>
#include <sunbright/native_render/j3d_specular_material.h>
#include <sunbright/native_render/j3d_unlit_material.h>
#include <sunbright/native_render/res_timg_decode.h>

#include <array>
#include <cstdint>

class J3DMaterial;
class J3DTexture;

namespace sb {

enum class NativeJ3dMaterialResult {
    Success,
    InvalidInput,
    UnsupportedProgram,
    MissingTexture,
    TextureDecodeFailure,
};

// The shared classifier's own result. This was a separate structure with the same four fields,
// filled by copying them across one at a time; the copy is what a shared boundary exists to avoid,
// and the only thing it dropped was the family name the classifier had already worked out.
using CapturedNativeJ3dMaterial = native_render::ClassifiedJ3dMaterial;

[[nodiscard]] bool
capture_native_j3d_material_state(J3DMaterial& material, bool hasVertexColor, bool hasNormal,
                                  native_render::J3dMaterialState& state) noexcept;
[[nodiscard]] NativeJ3dMaterialResult
capture_native_j3d_material(J3DMaterial& material, J3DTexture* textureTable, bool hasVertexColor,
                            bool hasNormal, CapturedNativeJ3dMaterial& captured,
                            native_render::ResTimgDecodeError& textureError) noexcept;

} // namespace sb
