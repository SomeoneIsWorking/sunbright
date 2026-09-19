// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <utility>

#include "bounded_tally.h"
#include "frame_draw_budget.h"
#include "guest_frame_renderer.h"

#include "gcnport/guest_context.h"
#include "gcnport/native_hooks.h"

namespace sunbright::gcnport_boot {

// Native hooks on the four GX entries that read the embedded framebuffer, and on the one that sets
// what a copy reads from.
//
// The renderer composes every draw of a frame into one image. The console does not: a title may
// render a pass, copy the result into a texture, clear the buffer and render the visible scene over
// the top, and each of those copies is a boundary the composition has to respect. A renderer with
// no counterpart for them draws an offscreen pass into the visible frame, which looks exactly like
// a blending defect and is not one.
//
// What this answers is where those boundaries fall: how many copies a frame makes, how many of the
// frame's draws precede each one, and what region each reads. A frame with no copy at all is a real
// answer and the one this has to be able to give, which is why the counts are printed whether or
// not any hook ever fired.
//
// Nothing is replaced: every entry ends in `call_original_once`, so the title copies exactly as it
// did.
class GuestEfbCopyProbe {
  public:
    // What the hooked address does. A copy to a texture and a copy to the display are different
    // boundaries -- one produces an input to a later draw, the other ends the frame -- and reading
    // them as one would lose the distinction this exists to make.
    enum class Entry : std::uint8_t { CopyToTexture, CopyToDisplay, SetTextureSource, SetClear };

    // `budget` is how the probe knows where in a frame a copy fell: it counts the draws the frame
    // has offered so far. It may be null, which reports copies without that position rather than
    // reporting a position of zero.
    //
    // `renderer` is what turns the measurement into composition. A copy to a texture is a pass
    // boundary, and the renderer is the owner that decides what one means; the probe only tells it
    // that the title reached one and whether the copy cleared. It may be null, in which case the
    // run observes the boundaries without acting on them.
    GuestEfbCopyProbe(Entry entry, const FrameDrawBudget* budget, GuestFrameRenderer* renderer,
                      std::uint64_t max_reports) noexcept
        : entry_(entry), budget_(budget), renderer_(renderer), maxReports_(max_reports) {}

    gcnport::HookResult operator()(gcnport::GuestContext& guest);

    void report() const;

    [[nodiscard]] static const char* entry_name(Entry entry) noexcept;

  private:
    static constexpr std::size_t MAX_DISTINCT_REGIONS = 64;
    // A frame's draw count is the title's, not this probe's, so the ordinals a copy can fall on are
    // bounded only by how much the title draws.
    static constexpr std::size_t MAX_DISTINCT_ORDINALS = 256;

    Entry entry_ = Entry::CopyToTexture;
    const FrameDrawBudget* budget_ = nullptr;
    GuestFrameRenderer* renderer_ = nullptr;
    std::uint64_t maxReports_ = 0;
    std::uint64_t entries_ = 0;
    std::uint64_t reports_ = 0;
    std::uint64_t clearing_ = 0;
    // Where in a frame each copy fell, by the number of draws the frame had already offered. This
    // is the measurement: a copy before every draw is a frame that begins by clearing, and a copy
    // partway through one is a pass that was never meant to be in the visible image.
    BoundedTally<std::uint64_t> drawsBefore_{MAX_DISTINCT_ORDINALS};
    // For `SetTextureSource`, the left/top and width/height a later copy will read.
    BoundedTally<std::pair<std::uint32_t, std::uint32_t>> regions_{MAX_DISTINCT_REGIONS};
};

} // namespace sunbright::gcnport_boot
