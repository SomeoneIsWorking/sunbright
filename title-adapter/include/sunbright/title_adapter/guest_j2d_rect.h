#pragma once

#include <sunbright/title_adapter/guest_memory.h>

#include <cstdint>

namespace sb::title_adapter {

// A `JUTRect`: the rectangle J2D states every bound, clip, viewport and logical screen in.
//
// Its edges are signed and its width is the difference between them, which is why it is a type
// rather than four numbers at each call site: a rectangle read as unsigned, or measured as
// `x2 - x1 + 1`, is off by a pixel or by four billion, and both look like a layout defect.
struct GuestRect {
    std::int32_t x1 = 0;
    std::int32_t y1 = 0;
    std::int32_t x2 = 0;
    std::int32_t y2 = 0;

    [[nodiscard]] std::int32_t width() const noexcept { return x2 - x1; }
    [[nodiscard]] std::int32_t height() const noexcept { return y2 - y1; }
    [[nodiscard]] bool empty() const noexcept { return width() <= 0 || height() <= 0; }
};

[[nodiscard]] bool read_guest_rect(const GuestReader& reader, GuestAddress address,
                                   GuestRect& out) noexcept;

} // namespace sb::title_adapter
