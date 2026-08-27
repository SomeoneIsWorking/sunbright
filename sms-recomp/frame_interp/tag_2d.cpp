// J2D pane interpolation.
//
// Orthographic geometry used to snap unconditionally on interpolated presentations. That is exact
// for a static HUD element, but not for an animating pane: native-60 updates its placement on every
// present, while interpolated-60 repeated each 30 Hz guest placement. J2D already supplies the
// missing identity at its draw seams: the persistent pane pointer. The draw path is included so a
// pane changing emitters cannot pair two unrelated primitive layouts.

#include "tag_2d.h"

#include "frame_interp.h"
#include "populations.h"

void sbr_gxfifo_draw_tag(uint64_t tag);
uint64_t sbr_gxfifo_pending_tag();
bool sbr_lerp_enabled();

namespace sb::frame_interp::two_d {
namespace {

uint64_t pane_tag(u32 pane, DrawPath path) {
    return (static_cast<uint64_t>(pane) << 32) | static_cast<u32>(path);
}

} // namespace

PaneScope::PaneScope(u32 pane, DrawPath path) : mPreviousPopulation(capture_population()) {
    // A caller-owned tag and population are one attribution unit. J2DPicture can be reached from
    // another interpolated owner (for example, a larger HUD draw); replacing only its population
    // would give that owner's tag the wrong graphics-DB classification.
    if (!sbr_lerp_enabled() || pane == 0 || sbr_gxfifo_pending_tag() != 0) {
        return;
    }

    sbr_gxfifo_draw_pop(SB_POP_J2D);
    sbr_gxfifo_draw_tag(pane_tag(pane, path));
    mTagged = true;
}

PaneScope::~PaneScope() {
    if (!mTagged)
        return;
    sbr_gxfifo_draw_tag(0);
    restore_population(mPreviousPopulation);
}

} // namespace sb::frame_interp::two_d
