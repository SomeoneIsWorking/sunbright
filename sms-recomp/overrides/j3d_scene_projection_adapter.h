#pragma once

#include "guest_byte_reader.h"

#include <sunbright/native_render/model_context.h>

#include <cstdint>

namespace sb::recomp {

enum class GuestJ3dSceneProjectionResult : std::uint8_t {
    Perspective,
    Orthographic,
    Unreadable,
    NonFinite,
    Unsupported,
};

// GMSE01 JDrama::TGraphics stores the camera's 4x4 projection at +0x74. Read the copied game
// value itself; the semantic renderer must not consult the GXSetProjection mirror or FIFO state.
[[nodiscard]] GuestJ3dSceneProjectionResult
capture_guest_j3d_scene_projection(const GuestByteReader& reader, std::uint32_t graphics,
                                   native_render::ModelSceneContext& context) noexcept;

} // namespace sb::recomp
