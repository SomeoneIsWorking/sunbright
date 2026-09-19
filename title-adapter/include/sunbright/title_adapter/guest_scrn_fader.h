#pragma once

#include <sunbright/title_adapter/guest_j2d_primitives.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace sb::title_adapter {

// The rectangles GMSE01's screen fader draws, as geometry rather than as GX calls.
//
// `TSMSFader::draw` is handed one rectangle -- the whole frame -- and turns it into either a single
// filled quad (`fill_rect`, 0x80140390) or a closing iris of four opaque bands (`draw_wipe_box`,
// 0x801400cc). Neither is a `J2DPane`, so neither carries bounds, a transform or a texture: the
// quads exist only as the vertices those two functions push, and this is where that geometry is
// stated once so a reader and a renderer cannot disagree about it.

// `draw_wipe_box` pushes sixteen vertices as four quads: a top band, a right band, a bottom band
// and a left band, each closing in on the centre as the fade colour's alpha rises. The bands come
// back in that order, in the same corner convention `GuestRect` uses everywhere else.
inline constexpr std::size_t GUEST_WIPE_BOX_BANDS = 4;

// The inset the guest computes: `alpha * (extent >> 1) / 255`, truncated to an integer. The shift
// is on the signed extent and the division is in single-precision float, both as the title wrote
// them, because an iris that closes one pixel off is a wipe that does not meet in the middle.
[[nodiscard]] std::int32_t wipe_box_inset(std::int32_t extent, std::uint8_t alpha) noexcept;

// Fills `bands` with the four rectangles and returns how many of them have any area. A band with
// none is still pushed by the guest and still rasterises to nothing, so it is reported rather than
// silently dropped: the count is how much of the iris is actually on screen.
[[nodiscard]] std::size_t
resolve_wipe_box_bands(const GuestRect& frame, std::uint8_t alpha,
                       std::array<GuestRect, GUEST_WIPE_BOX_BANDS>& bands) noexcept;

} // namespace sb::title_adapter
