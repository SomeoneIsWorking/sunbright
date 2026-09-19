// Drives the shipping fader geometry against what `draw_wipe_box` pushes.
//
// The interesting case is not that four rectangles come back; it is that they are the four the
// guest's sixteen vertices describe. A border of uniform width would pass a "thin frame" check and
// still be wrong, because the guest's bands overlap asymmetrically -- each takes one of its edges
// from the inner rectangle and the opposite one from the frame.

#include <sunbright/title_adapter/guest_scrn_fader.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace {

using sb::title_adapter::GUEST_WIPE_BOX_BANDS;
using sb::title_adapter::GuestRect;
using sb::title_adapter::resolve_wipe_box_bands;
using sb::title_adapter::wipe_box_inset;

// The frame `TApplication::gameLoop` hands the fader.
constexpr GuestRect FRAME{0, 0, 640, 448};

std::int64_t area(const GuestRect& rect) {
    return rect.empty()
               ? 0
               : static_cast<std::int64_t>(rect.width()) * static_cast<std::int64_t>(rect.height());
}

void the_inset_is_the_guest_arithmetic() {
    // 255 * 320 / 255 is the full half-extent; one less alpha is one step short of it.
    assert(wipe_box_inset(640, 255) == 320);
    assert(wipe_box_inset(448, 255) == 224);
    assert(wipe_box_inset(640, 0) == 0);
    // Truncation, not rounding: 1 * 320 / 255 is 1.254, and 200 * 320 / 255 is 250.98.
    assert(wipe_box_inset(640, 1) == 1);
    assert(wipe_box_inset(640, 200) == 250);
    // The shift is on the extent, so an odd extent loses its last pixel before scaling.
    assert(wipe_box_inset(641, 255) == 320);
}

void a_closed_iris_tiles_the_frame() {
    std::array<GuestRect, GUEST_WIPE_BOX_BANDS> bands{};
    assert(resolve_wipe_box_bands(FRAME, 255, bands) == GUEST_WIPE_BOX_BANDS);

    // Four quadrants, meeting at the centre and covering the frame exactly once.
    assert((bands[0] == GuestRect{0, 0, 320, 224}));
    assert((bands[1] == GuestRect{320, 0, 640, 224}));
    assert((bands[2] == GuestRect{320, 224, 640, 448}));
    assert((bands[3] == GuestRect{0, 224, 320, 448}));
    std::int64_t covered = 0;
    for (const GuestRect& band : bands) {
        covered += area(band);
    }
    assert(covered == area(FRAME));
}

void an_opening_iris_is_a_thin_frame() {
    std::array<GuestRect, GUEST_WIPE_BOX_BANDS> bands{};
    assert(resolve_wipe_box_bands(FRAME, 2, bands) == GUEST_WIPE_BOX_BANDS);

    // Two pixels in from the sides and one from the top: each band takes its thickness from its
    // own axis and its length from the far edge of the frame, so the four meet without a seam.
    assert((bands[0] == GuestRect{0, 0, 638, 1}));
    assert((bands[1] == GuestRect{638, 0, 640, 447}));
    assert((bands[2] == GuestRect{2, 447, 640, 448}));
    assert((bands[3] == GuestRect{0, 1, 2, 448}));
}

// The iris does not close as a square. Each axis insets by its own half-extent, so on a 640x448
// frame the sides move 320/448ths faster than the top and bottom, and the first alpha at which the
// sides have moved at all is one at which the top and bottom have not.
void the_two_axes_close_at_their_own_rates() {
    std::array<GuestRect, GUEST_WIPE_BOX_BANDS> bands{};
    assert(wipe_box_inset(FRAME.width(), 1) == 1);
    assert(wipe_box_inset(FRAME.height(), 1) == 0);
    assert(resolve_wipe_box_bands(FRAME, 1, bands) == 2);
    assert(bands[0].empty() && bands[2].empty());
    assert((bands[1] == GuestRect{639, 0, 640, 448}));
    assert((bands[3] == GuestRect{0, 0, 1, 448}));
}

void a_fully_open_iris_covers_nothing() {
    std::array<GuestRect, GUEST_WIPE_BOX_BANDS> bands{};
    // `TSMSFader::drawFadeinout` never calls this at zero alpha, but the geometry still has to say
    // that nothing is covered rather than return four frame-sized quads.
    assert(resolve_wipe_box_bands(FRAME, 0, bands) == 0);
    for (const GuestRect& band : bands) {
        assert(band.empty());
    }
}

} // namespace

int main() {
    the_inset_is_the_guest_arithmetic();
    a_closed_iris_tiles_the_frame();
    an_opening_iris_is_a_thin_frame();
    the_two_axes_close_at_their_own_rates();
    a_fully_open_iris_covers_nothing();
    return 0;
}
