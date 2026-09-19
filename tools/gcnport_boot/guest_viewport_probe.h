// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <tuple>

#include "bounded_tally.h"
#include "frame_draw_budget.h"

#include "gcnport/guest_context.h"
#include "gcnport/native_hooks.h"
#include "guest_screen_space.h"

namespace sunbright::gcnport_boot {

// Native hooks on the two GX entries that say which part of the framebuffer a draw may reach.
//
// This renderer rasterises every draw against the whole target, which is right only while the title
// does. Measuring the framebuffer copies established that GMSE01 does not: it copies a 256x256
// region out of a 640x448 buffer, so at least one of its passes is authored to fill a region rather
// than the screen. A draw confined to a corner on the console and spread over the whole frame here
// covers geometry it was never meant to touch, which is indistinguishable from a shading defect in
// the finished image.
//
// What this answers is which rectangles the title sets and where in a frame it sets them: a
// rectangle set before the frame's first draw governs the whole frame, and one set partway through
// governs only what follows. Both the viewport and the scissor are read, because they are separate
// authored values -- the viewport maps clip space onto the region and the scissor discards outside
// it -- and a renderer that implemented one would still be wrong about the other.
//
// Nothing is replaced: every entry ends in `call_original_once`.
class GuestViewportProbe {
  public:
    // Which hooked entry this is. The two take their arguments in different register files, which
    // is the whole reason a probe has to be told rather than infer.
    enum class Entry : std::uint8_t { Viewport, Scissor };

    // `screen_space` may be null, and is fed only by the viewport entry: the scissor discards
    // outside a region, it does not say what a coordinate means.
    GuestViewportProbe(Entry entry, const FrameDrawBudget* budget, GuestScreenSpace* screen_space,
                       std::uint64_t max_reports) noexcept
        : entry_(entry), budget_(budget), screenSpace_(screen_space), maxReports_(max_reports) {}

    gcnport::HookResult operator()(gcnport::GuestContext& guest);

    void report() const;

    [[nodiscard]] static const char* entry_name(Entry entry) noexcept;

  private:
    // Left, top, width and height, in the units the entry states them: whole pixels for the
    // scissor, and for the viewport the float values rounded to the nearest pixel after being
    // checked against the value they came from, so a fractional rectangle is reported as one rather
    // than quietly truncated.
    using Rectangle = std::tuple<std::int32_t, std::int32_t, std::int32_t, std::int32_t>;

    static constexpr std::size_t MAX_DISTINCT_RECTANGLES = 64;
    static constexpr std::size_t MAX_DISTINCT_ORDINALS = 128;

    Entry entry_ = Entry::Viewport;
    const FrameDrawBudget* budget_ = nullptr;
    GuestScreenSpace* screenSpace_ = nullptr;
    std::uint64_t maxReports_ = 0;
    std::uint64_t entries_ = 0;
    std::uint64_t reports_ = 0;
    // A viewport whose left/top/width/height are not whole pixels. GX takes floats and the title
    // could author a fractional region; reporting the rounded rectangle without saying how many
    // were rounded would make an unsupported case look like a supported one.
    std::uint64_t fractional_ = 0;
    // The depth range, which the viewport carries alongside the region and which nothing here
    // rasterises yet. Kept as a range rather than a set: any value but the full 0..1 is the
    // finding.
    double nearestZ_ = 0.0;
    double farthestZ_ = 0.0;
    bool sawDepth_ = false;
    BoundedTally<Rectangle> rectangles_{MAX_DISTINCT_RECTANGLES};
    // How many of the frame's draws had been offered when the rectangle was set. Zero is a
    // rectangle governing the whole frame; anything else splits the frame into regions.
    BoundedTally<std::uint64_t> drawsBefore_{MAX_DISTINCT_ORDINALS};
};

} // namespace sunbright::gcnport_boot
