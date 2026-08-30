#pragma once

#include <sunbright/native_render/solid_rectangle.h>

#include "guest_byte_reader.h"

#include <cstdint>

namespace sb::recomp {

// GC2D fill_rect receives a guest JDrama::TRect in r3 and packed RGBA8 in r4. Capture the final
// values at that high-level seam; no GX state is consulted or reproduced here.
[[nodiscard]] bool capture_fill_rectangle(const GuestByteReader& reader, std::uint32_t rect,
                                          std::uint32_t rgba,
                                          native_render::SolidRectangleDraw& draw) noexcept;

} // namespace sb::recomp
