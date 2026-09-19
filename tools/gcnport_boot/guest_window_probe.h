// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <map>

#include <sunbright/native_render/window.h>
#include <sunbright/title_adapter/guest_j2d_window.h>

#include "frame_draw_budget.h"
#include "guest_j2d_context_probe.h"
#include "guest_texture_cache.h"

#include "gcnport/guest_context.h"
#include "gcnport/native_hooks.h"

namespace sunbright::gcnport_boot {

// Publishes the framed panels a `J2DWindow` draws.
//
// A window is the panel almost every piece of GMSE01's text is written onto, and until now it was
// the one 2D producer with no publisher: its glyphs were drawn and the panel under them was not,
// so a published frame showed text floating over whatever happened to be behind it.
//
// It sits on `J2DWindow::draw_private` (0x802d18ec) rather than on either `drawSelf`. That is the
// single point both overloads funnel into, and it is where the outer rectangle, the contents
// rectangle and the parent transform are all arguments in hand -- `drawSelf(int, int)` builds an
// identity matrix on its own stack and passes a pointer to it, which is readable there and gone by
// the time the body returns.
//
// Nothing here decides what a window means. `resolve_window_layout` owns the corner placement, the
// stretched edge pieces and the mirrored texture coordinates, and `make_window_contents_command`
// and `make_window_picture_command` own how those become the renderer's existing primitives; a
// window needs no window-specific shader, only the right ordered composition.
class GuestWindowProbe {
  public:
    // `context` may be null, which is a run that did not ask for the screen to be followed; every
    // window is then counted as having no canvas rather than published into a guessed one.
    // `budget` may be null, which is an unbounded run.
    GuestWindowProbe(const GuestJ2dContextProbe* context, FrameDrawBudget* budget,
                     std::uint64_t max_reports) noexcept
        : context_(context), budget_(budget), maxReports_(max_reports) {}

    gcnport::HookResult operator()(gcnport::GuestContext& guest);

    void report() const;

  private:
    [[nodiscard]] bool publish_part(gcnport::GuestContext& guest,
                                    const sb::native_render::WindowTexturedPart& part,
                                    const sb::title_adapter::GuestJutTexture& texture,
                                    const sb::native_render::PictureContext& context,
                                    const sb::title_adapter::GuestWindow& window,
                                    sb::native_render::Color black, sb::native_render::Color white);

    const GuestJ2dContextProbe* context_ = nullptr;
    FrameDrawBudget* budget_ = nullptr;
    std::uint64_t maxReports_ = 0;
    std::uint64_t reports_ = 0;

    std::uint64_t entries_ = 0;
    std::uint64_t withoutCanvas_ = 0;
    std::uint64_t unreadableRectangles_ = 0;
    std::uint64_t unreadableTransform_ = 0;
    std::uint64_t withheldByBudget_ = 0;
    std::uint64_t withoutSink_ = 0;
    // Windows retail itself declines to draw because the rectangle handed to `draw_private` is
    // narrower or shorter than the window's stated minimum. Counted rather than dropped silently,
    // because a publisher reporting nothing and a title drawing nothing look identical otherwise.
    std::uint64_t culledBySize_ = 0;
    std::uint64_t rejectedLayout_ = 0;
    std::uint64_t framelessWindows_ = 0;
    std::uint64_t contentsFills_ = 0;
    std::uint64_t contentsTextures_ = 0;
    std::uint64_t frameParts_ = 0;
    std::uint64_t submitted_ = 0;
    std::uint64_t acceptedBySink_ = 0;
    std::map<sb::title_adapter::GuestWindowError, std::uint64_t> windowErrors_;
    GuestTextureCache textures_;
};

} // namespace sunbright::gcnport_boot
