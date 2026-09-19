#pragma once

#include <sunbright/title_adapter/guest_memory.h>

#include <array>
#include <cstdint>

namespace sb::title_adapter {

// The two values every J2D reader is built out of: the rectangle J2D states its geometry in, and
// the 3x4 matrix it places that geometry with. They are here rather than in whichever reader
// happened to need one first so that a pane, a graphics context and a window all mean the same
// thing by a rectangle and by a transform.

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

    bool operator==(const GuestRect&) const = default;
};

[[nodiscard]] bool read_guest_rect(const GuestReader& reader, GuestAddress address,
                                   GuestRect& out) noexcept;

// One `Mtx`: row-major 3x4, in the order the guest stores it, so a caller copies rather than
// transposes.
[[nodiscard]] bool read_guest_matrix(const GuestReader& reader, GuestAddress address,
                                     std::array<float, 12>& out) noexcept;

// The same, from a raw reader and guest pointer -- the form a hook has when the matrix it was
// handed lives on the guest stack rather than inside an object it can reach.
[[nodiscard]] bool read_guest_matrix(const GuestMemory& memory, GuestAddress matrix,
                                     std::array<float, 12>& out) noexcept;

} // namespace sb::title_adapter
