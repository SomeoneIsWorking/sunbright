#pragma once

#include <sunbright/title_adapter/guest_memory.h>

namespace sb::title_adapter {

// `J2DPane`, the base every J2D screen element extends.
//
// A pane is not read on its own -- nothing draws one -- but its fields are where a derived reader
// finds the rectangle it covers, the transform it was placed by, and the alpha its parent handed
// down. Two of those readers exist now (`J2DPicture` and `J2DWindow`), so the offsets they share
// are stated once here rather than once per subclass.

inline constexpr GuestAddress GUEST_PANE_BOUNDS = 0x14;
inline constexpr GuestAddress GUEST_PANE_GLOBAL_BOUNDS = 0x24;
inline constexpr GuestAddress GUEST_PANE_CLIP_RECT = 0x34;
inline constexpr GuestAddress GUEST_PANE_POSITION_MATRIX = 0x54;
inline constexpr GuestAddress GUEST_PANE_GLOBAL_MATRIX = 0x84;
inline constexpr GuestAddress GUEST_PANE_COLOR_ALPHA = 0xCD;

} // namespace sb::title_adapter
