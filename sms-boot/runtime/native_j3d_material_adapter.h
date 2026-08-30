#pragma once

#include <sunbright/native_render/j3d_unlit_material.h>
#include <sunbright/native_render/res_timg_decode.h>

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

struct CapturedNativeJ3dMaterial {
    native_render::ModelMaterial material{};
    native_render::DecodedTexture texture{};
    bool hasTexture = false;
};

[[nodiscard]] bool
capture_native_j3d_material_state(J3DMaterial& material, bool hasVertexColor,
                                  native_render::J3dUnlitMaterialState& state) noexcept;
[[nodiscard]] NativeJ3dMaterialResult
capture_native_j3d_material(J3DMaterial& material, J3DTexture* textureTable, bool hasVertexColor,
                            CapturedNativeJ3dMaterial& captured,
                            native_render::ResTimgDecodeError& textureError) noexcept;

} // namespace sb
