// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>

#include <sunbright/native_render/solid_rectangle.h>
#include <sunbright/title_adapter/guest_j2d_graf_context.h>
#include <sunbright/title_adapter/guest_scrn_fader.h>

#include "frame_draw_budget.h"
#include "guest_j2d_context_probe.h"
#include "guest_matrix_state.h"
#include "guest_screen_space.h"

#include "gcnport/guest_context.h"
#include "gcnport/native_hooks.h"

namespace sunbright::gcnport_boot {

// Publishes the flat-coloured quads GMSE01 draws over the frame.
//
// Everything a player sees between two scenes is one of these. `TSMSFader` fills the frame with
// the fade colour or closes an iris of four bands over it, every frame of every transition, and
// `J2DGrafContext::fillBox` fills a box under a J2D screen. None of them is a `J2DPicture` or a
// `J3DShape`, so a run that published models and panes rendered every transition as an abrupt cut.
//
// The three entries differ in where their screen comes from, which is the whole reason they are one
// probe rather than three. A `fillBox` is inside a `J2DGrafContext` that states its own screen and
// viewport; the fader's quads are in whatever orthographic projection and viewport GX was last
// given, because `TApplication::gameLoop` sets them itself and hands the fader only a rectangle.
class GuestSolidRectangleProbe {
  public:
    enum class Entry : std::uint8_t {
        // `fill_rect` (0x80140390): one quad over the rectangle, in the fade colour.
        FadeRect,
        // `draw_wipe_box` (0x801400cc): four opaque bands closing in on the centre.
        WipeBox,
        // `J2DGrafContext::fillBox` (0x802eba70): one quad, four corner colours, placed by the
        // context's own matrix.
        FillBox,
    };

    // `screen_space`, `context` and `matrices` may each be null, which is a run that did not ask
    // for that state to be followed; a rectangle is then counted as having no canvas or no
    // placement rather than published into a guessed one. `budget` may be null, which is an
    // unbounded run.
    GuestSolidRectangleProbe(Entry entry, const GuestScreenSpace* screen_space,
                             const GuestJ2dContextProbe* context, const GuestMatrixState* matrices,
                             FrameDrawBudget* budget, std::uint64_t max_reports) noexcept
        : entry_(entry), screenSpace_(screen_space), context_(context), matrices_(matrices),
          budget_(budget), maxReports_(max_reports) {}

    gcnport::HookResult operator()(gcnport::GuestContext& guest);

    void report() const;

    [[nodiscard]] static const char* entry_name(Entry entry) noexcept;

  private:
    // One quad, already in the corner convention `SolidRectangleCommand` uses.
    struct Quad {
        sb::title_adapter::GuestRect rect{};
        std::array<sb::native_render::Color, 4> corner{};
        std::uint64_t instance = 0;
    };

    [[nodiscard]] bool
    read_fader_quads(gcnport::GuestContext& guest,
                     std::array<Quad, sb::title_adapter::GUEST_WIPE_BOX_BANDS>& quads,
                     std::size_t& count);
    [[nodiscard]] bool read_fill_box_quad(gcnport::GuestContext& guest, Quad& quad,
                                          sb::native_render::Matrix3x4& transform);
    void publish(const sb::native_render::Canvas& canvas, const sb::native_render::ClipRect& clip,
                 const Quad& quad, const sb::native_render::Matrix3x4& transform);

    Entry entry_ = Entry::FadeRect;
    const GuestScreenSpace* screenSpace_ = nullptr;
    const GuestJ2dContextProbe* context_ = nullptr;
    const GuestMatrixState* matrices_ = nullptr;
    FrameDrawBudget* budget_ = nullptr;
    std::uint64_t maxReports_ = 0;
    std::uint64_t reports_ = 0;

    std::uint64_t entries_ = 0;
    std::uint64_t unreadableArguments_ = 0;
    std::uint64_t withoutCanvas_ = 0;
    // A fader quad entered before any position matrix had been followed. `fillBox` is not counted
    // here: it carries its context's own matrix as a field and needs nothing from GX state.
    std::uint64_t withoutMatrix_ = 0;
    std::uint64_t withheldByBudget_ = 0;
    // Quads the guest pushes that enclose no pixels: an iris band at the alpha where its axis has
    // not started to close, or a box whose rectangle is empty. They rasterise to nothing on the
    // console too, so this is a measurement rather than a loss.
    std::uint64_t withoutArea_ = 0;
    std::uint64_t unresolvedPositions_ = 0;
    std::uint64_t withoutSink_ = 0;
    std::uint64_t submitted_ = 0;
    std::uint64_t acceptedBySink_ = 0;
    std::map<sb::title_adapter::GuestGrafContextError, std::uint64_t> contextErrors_;
};

} // namespace sunbright::gcnport_boot
