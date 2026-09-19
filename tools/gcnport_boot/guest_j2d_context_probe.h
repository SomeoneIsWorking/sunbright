// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <map>

#include <sunbright/native_render/picture_context.h>
#include <sunbright/title_adapter/guest_j2d_graf_context.h>

#include "gcnport/guest_context.h"
#include "gcnport/native_hooks.h"

namespace sunbright::gcnport_boot {

// Follows which `J2DOrthoGraph` the title is drawing 2D into.
//
// A pane's bounds are coordinates in a logical screen, and the screen they are in is not in the
// pane. It is in the graf context the title set up before it started drawing, and a picture
// published without one would be a quad with a size and no space -- which is not a small error: the
// title's 640x480 screen and the 608x448 viewport it rasterises into differ by enough that
// everything would land in the wrong place while still looking like a plausible frame.
//
// It sits on `J2DGrafContext::setup2D` (0x802eb6bc) rather than on `J2DScreen::draw`, because
// setup2D is where the context takes effect for everything that follows, and it is reached from
// both of J2DScreen::draw's branches -- the caller-supplied context and the 640x480 one the screen
// builds for itself. This is the same seam the decomp runtime activates its context at.
//
// What it does not do is nest. `J2DScreen::draw` pushes a context for the subtree beneath it and
// restores the previous one on the way out; this keeps the last one activated. That is the same
// answer whenever a screen is not drawn inside another screen's traversal, and `nested_screens`
// counts the times it would not be, so the gap is a number rather than an assumption.
class GuestJ2dContextProbe {
  public:
    explicit GuestJ2dContextProbe(std::uint64_t max_reports) noexcept : maxReports_(max_reports) {}

    gcnport::HookResult operator()(gcnport::GuestContext& guest);

    // The context in force, or null when the title has not established one this run.
    [[nodiscard]] const sb::native_render::PictureContext* current() const noexcept {
        return has_ ? &current_ : nullptr;
    }

    void report() const;

  private:
    std::uint64_t maxReports_ = 0;
    std::uint64_t entries_ = 0;
    std::uint64_t reports_ = 0;
    std::uint64_t accepted_ = 0;
    std::uint64_t invalidCanvas_ = 0;
    std::uint64_t changes_ = 0;
    std::map<sb::title_adapter::GuestGrafContextError, std::uint64_t> errors_;
    sb::native_render::PictureContext current_{};
    bool has_ = false;
};

} // namespace sunbright::gcnport_boot
