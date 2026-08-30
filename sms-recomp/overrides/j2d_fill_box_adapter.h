#pragma once

#include <sunbright/native_render/picture_context.h>
#include <sunbright/native_render/solid_rectangle.h>

#include "guest_byte_reader.h"

#include <cstdint>

namespace sb::recomp {

// Capture J2DGrafContext::fillBox at its high-level boundary. The retail body narrows all four
// rectangle coordinates to signed 16-bit, transforms them through mPosMtx, and deliberately emits
// mColorBL at the geometric bottom-right and mColorBR at bottom-left.
[[nodiscard]] bool capture_j2d_fill_box(const GuestByteReader& reader, std::uint32_t self,
                                        std::uint32_t rect,
                                        const native_render::PictureContext& context,
                                        native_render::SolidRectangleDraw& draw) noexcept;

} // namespace sb::recomp
