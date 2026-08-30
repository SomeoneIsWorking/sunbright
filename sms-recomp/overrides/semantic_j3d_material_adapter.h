#pragma once

#include "guest_byte_reader.h"

#include <sunbright/native_render/j3d_unlit_material.h>

#include <cstdint>

namespace sb::recomp {

[[nodiscard]] bool
capture_guest_j3d_material_state(const GuestByteReader& reader, std::uint32_t material,
                                 bool hasVertexColor,
                                 native_render::J3dUnlitMaterialState& state) noexcept;

} // namespace sb::recomp
