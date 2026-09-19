// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <set>

#include <sunbright/title_adapter/guest_projection.h>

#include "gcnport/guest_context.h"
#include "gcnport/native_hooks.h"
#include "guest_screen_space.h"

namespace sunbright::gcnport_boot {

// A native hook on `GXSetProjection` (0x80362c34) that reads the matrix GMSE01 is about to give the
// hardware and publishes it through `native_render::publish_j3d_projection`.
//
// A `ModelDraw` carries a projection, and nothing was supplying one. This is the seam that has it:
// the entry takes a pointer to the matrix in r3 and the projection type in r4, which is every value
// the reader needs. Taking it here rather than from a camera object means the probe sees exactly
// what was submitted, including any projection a camera never owned.
//
// The reader refuses a matrix whose discarded entries are not the ones the hardware would have
// supplied, so a count of refusals here is a real finding about the title rather than a reader
// fault. Nothing is replaced: every entry ends in `call_original_once`.
class GuestProjectionProbe {
  public:
    // `screen_space` may be null: the probe's own job is publishing a 3D projection, and telling
    // the screen space what the orthographic ones mean is a second consumer of the same read.
    GuestProjectionProbe(GuestScreenSpace* screen_space, std::uint64_t max_reports) noexcept
        : screenSpace_(screen_space), maxReports_(max_reports) {}

    gcnport::HookResult operator()(gcnport::GuestContext& guest);

    void report() const;

  private:
    // A title uses a handful of distinct projections: a scene camera, a HUD, and little else.
    static constexpr std::size_t MAX_DISTINCT_PROJECTIONS = 64;

    GuestScreenSpace* screenSpace_ = nullptr;
    std::uint64_t maxReports_ = 0;
    std::uint64_t entries_ = 0;
    std::uint64_t reports_ = 0;
    std::uint64_t published_ = 0;
    std::uint64_t distinctPast_ = 0;
    std::map<sb::title_adapter::GuestProjectionError, std::uint64_t> errors_;
    std::map<sb::title_adapter::GuestProjectionKind, std::uint64_t> kinds_;
    std::map<sb::title_adapter::GuestOrthographicScreenError, std::uint64_t> screenErrors_;
    std::set<std::uint64_t> distinct_;
};

} // namespace sunbright::gcnport_boot
