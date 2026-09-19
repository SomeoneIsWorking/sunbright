// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_solid_rectangle_probe.h"

#include <sunbright/native_render/semantic_sink.h>

#include <algorithm>
#include <cstdio>
#include <span>

namespace sunbright::gcnport_boot {
namespace {

// `fill_rect(const TRect&, TColor)` and `draw_wipe_box(const TRect&, TColor)` are free functions
// taking a rectangle in r3 and a colour in r4. CodeWarrior passes a four-byte aggregate by address
// rather than by value, so r4 is a pointer to the colour and not the colour itself -- confirmed in
// the retail image, where `fill_rect` reads its alpha with `lbz r0, 3(r31)` after `addi r31, r4, 0`
// and then loads the whole word with `lwz r31, 0(r31)`.
constexpr std::size_t FIRST_ARGUMENT_REGISTER = 3;
constexpr std::size_t SECOND_ARGUMENT_REGISTER = 4;

// `draw_wipe_box` pushes `u32 color = 0xff` for all sixteen of its vertices: opaque black,
// whatever the fade colour is. The colour it is handed sets how far the iris has closed, not what
// the bands are painted with.
constexpr std::uint32_t WIPE_BOX_BAND_COLOR = 0x000000FF;

bool read_through_guest_context(sb::title_adapter::GuestAddress address,
                                std::span<std::uint8_t> destination, void* context) {
    auto* const guest = static_cast<gcnport::GuestContext*>(context);
    return guest->read_memory(address, std::as_writable_bytes(destination));
}

} // namespace

const char* GuestSolidRectangleProbe::entry_name(Entry entry) noexcept {
    switch (entry) {
    case Entry::FadeRect:
        return "fade rect";
    case Entry::WipeBox:
        return "wipe box";
    case Entry::FillBox:
        return "fill box";
    }
    return "unknown";
}

bool GuestSolidRectangleProbe::read_fader_quads(
    gcnport::GuestContext& guest, std::array<Quad, sb::title_adapter::GUEST_WIPE_BOX_BANDS>& quads,
    std::size_t& count) {
    const auto rectAddress = static_cast<sb::title_adapter::GuestAddress>(
        guest.general_register(FIRST_ARGUMENT_REGISTER));
    const auto colorAddress = static_cast<sb::title_adapter::GuestAddress>(
        guest.general_register(SECOND_ARGUMENT_REGISTER));
    const sb::title_adapter::GuestMemory memory{read_through_guest_context, &guest};
    const sb::title_adapter::GuestReader reader(memory);

    sb::title_adapter::GuestRect frame{};
    std::uint32_t rgba = 0;
    if (rectAddress == 0 || colorAddress == 0 ||
        !sb::title_adapter::read_guest_rect(reader, rectAddress, frame) ||
        !reader.word(colorAddress, rgba)) {
        return false;
    }

    if (entry_ == Entry::FadeRect) {
        const sb::native_render::Color color = sb::native_render::color_from_rgba8(rgba);
        quads[0] = {frame, {color, color, color, color}, rectAddress};
        count = 1;
        return true;
    }

    std::array<sb::title_adapter::GuestRect, sb::title_adapter::GUEST_WIPE_BOX_BANDS> bands{};
    const auto alpha = static_cast<std::uint8_t>(rgba & 0xFFU);
    (void)sb::title_adapter::resolve_wipe_box_bands(frame, alpha, bands);
    const sb::native_render::Color band = sb::native_render::color_from_rgba8(WIPE_BOX_BAND_COLOR);
    for (std::size_t index = 0; index < bands.size(); ++index) {
        // The band's ordinal joins the rectangle's address so four quads drawn from one call are
        // four instances rather than one repeated.
        quads[index] = {bands[index],
                        {band, band, band, band},
                        (static_cast<std::uint64_t>(rectAddress) << 8U) | index};
    }
    count = bands.size();
    return true;
}

bool GuestSolidRectangleProbe::read_fill_box_quad(gcnport::GuestContext& guest, Quad& quad,
                                                  sb::native_render::Matrix3x4& transform) {
    const auto contextAddress = static_cast<sb::title_adapter::GuestAddress>(
        guest.general_register(FIRST_ARGUMENT_REGISTER));
    const auto boxAddress = static_cast<sb::title_adapter::GuestAddress>(
        guest.general_register(SECOND_ARGUMENT_REGISTER));
    const sb::title_adapter::GuestMemory memory{read_through_guest_context, &guest};
    const sb::title_adapter::GuestReader reader(memory);

    sb::title_adapter::GuestRect box{};
    if (boxAddress == 0 || !sb::title_adapter::read_guest_rect(reader, boxAddress, box)) {
        return false;
    }
    sb::title_adapter::GuestGrafContextFill fill{};
    const sb::title_adapter::GuestGrafContextError error =
        sb::title_adapter::read_guest_graf_context_fill(memory, contextAddress, fill);
    contextErrors_[error] += 1;
    if (error != sb::title_adapter::GuestGrafContextError::None) {
        return false;
    }

    // J2D's corner names are not the geometry's: `fillBox` emits (x2,y2) with `mColorBL` and
    // (x1,y2) with `mColorBR`, so the two bottom colours swap on the way into the geometric
    // TL, TR, BL, BR order the command uses.
    quad = {box,
            {sb::native_render::color_from_rgba8(fill.colorTL),
             sb::native_render::color_from_rgba8(fill.colorTR),
             sb::native_render::color_from_rgba8(fill.colorBR),
             sb::native_render::color_from_rgba8(fill.colorBL)},
            contextAddress};
    std::copy_n(fill.positionMatrix.begin(), fill.positionMatrix.size(), transform.value.begin());
    return true;
}

void GuestSolidRectangleProbe::publish(const sb::native_render::Canvas& canvas,
                                       const sb::native_render::ClipRect& clip, const Quad& quad,
                                       const sb::native_render::Matrix3x4& transform) {
    if (quad.rect.empty()) {
        withoutArea_ += 1;
        return;
    }
    if (budget_ != nullptr && !budget_->take()) {
        withheldByBudget_ += 1;
        return;
    }

    sb::native_render::SolidRectangleDraw draw{};
    draw.canvas = canvas;
    draw.rectangle.instance = quad.instance;
    draw.rectangle.source = entry_ == Entry::FillBox
                                ? sb::native_render::SolidRectangleSource::J2dGrafContextFillBox
                                : sb::native_render::SolidRectangleSource::Gc2dFillRect;
    draw.rectangle.corner = quad.corner;
    draw.rectangle.clip = clip;
    const sb::native_render::TransformedS16RectangleLayout layout{
        quad.rect.x1, quad.rect.y1, quad.rect.x2, quad.rect.y2, transform};
    if (!sb::native_render::resolve_transformed_s16_rectangle(layout, draw.rectangle.positions) ||
        !sb::native_render::valid(draw)) {
        unresolvedPositions_ += 1;
        return;
    }

    if (reports_ < maxReports_) {
        reports_ += 1;
        // The frame ordinal is what makes a reported quad reachable: a transition is a run of
        // frames, and dumping one of them is the only way to see whether the pass rasterised it.
        std::printf("gmse01_boot: frame %llu: %s %d,%d..%d,%d rgba=(%g, %g, %g, %g) in %gx%g at "
                    "(%g, %g)\n",
                    static_cast<unsigned long long>(budget_ != nullptr ? budget_->frames() : 0),
                    entry_name(entry_), quad.rect.x1, quad.rect.y1, quad.rect.x2, quad.rect.y2,
                    static_cast<double>(quad.corner[0].r), static_cast<double>(quad.corner[0].g),
                    static_cast<double>(quad.corner[0].b), static_cast<double>(quad.corner[0].a),
                    static_cast<double>(canvas.extent.x), static_cast<double>(canvas.extent.y),
                    static_cast<double>(canvas.origin.x), static_cast<double>(canvas.origin.y));
    }

    if (!sb::native_render::has_semantic_sink()) {
        withoutSink_ += 1;
        return;
    }
    submitted_ += 1;
    if (sb::native_render::submit_solid_rectangle(draw)) {
        acceptedBySink_ += 1;
    }
}

gcnport::HookResult GuestSolidRectangleProbe::operator()(gcnport::GuestContext& guest) {
    entries_ += 1;

    std::array<Quad, sb::title_adapter::GUEST_WIPE_BOX_BANDS> quads{};
    std::size_t count = 0;
    sb::native_render::Matrix3x4 transform{};
    sb::native_render::Canvas canvas{};
    sb::native_render::ClipRect clip{};

    if (entry_ == Entry::FillBox) {
        const sb::native_render::PictureContext* const context =
            context_ != nullptr ? context_->current() : nullptr;
        if (context == nullptr) {
            withoutCanvas_ += 1;
            return gcnport::HookResult::call_original_once();
        }
        canvas = context->canvas;
        clip = context->scissor;
        if (!read_fill_box_quad(guest, quads[0], transform)) {
            unreadableArguments_ += 1;
            return gcnport::HookResult::call_original_once();
        }
        count = 1;
    } else {
        const sb::native_render::Canvas* const screen =
            screenSpace_ != nullptr ? screenSpace_->current() : nullptr;
        if (screen == nullptr) {
            withoutCanvas_ += 1;
            return gcnport::HookResult::call_original_once();
        }
        canvas = *screen;
        // `TSMSFader::setupGraphicsFadeinout` loads `MTXTrans(m, 0, 0, 0)` into GX_PNMTX0 and makes
        // it current before either fader entry draws, so this is expected to be the identity. It is
        // read rather than assumed: a quad placed by a matrix nobody watched is a frame that looks
        // plausible and is not the title's.
        const sb::native_render::Matrix3x4* const placement =
            matrices_ != nullptr ? matrices_->current() : nullptr;
        if (placement == nullptr) {
            withoutMatrix_ += 1;
            return gcnport::HookResult::call_original_once();
        }
        transform = *placement;
        if (!read_fader_quads(guest, quads, count)) {
            unreadableArguments_ += 1;
            return gcnport::HookResult::call_original_once();
        }
    }

    for (std::size_t index = 0; index < count; ++index) {
        publish(canvas, clip, quads[index], transform);
    }
    return gcnport::HookResult::call_original_once();
}

void GuestSolidRectangleProbe::report() const {
    std::printf("gmse01_boot: guest %s probe: %llu call(s), %llu quad(s) submitted, %llu accepted "
                "by the sink\n",
                entry_name(entry_), static_cast<unsigned long long>(entries_),
                static_cast<unsigned long long>(submitted_),
                static_cast<unsigned long long>(acceptedBySink_));
    if (entries_ == 0) {
        // A transition the run never reached and a hook that never installed report the same zero.
        std::printf("gmse01_boot:   the hook installed and the title never reached it\n");
        return;
    }
    std::printf("gmse01_boot:   not published: no_canvas=%llu no_matrix=%llu "
                "unreadable_arguments=%llu no_area=%llu withheld_by_budget=%llu "
                "unresolved_positions=%llu no_sink=%llu\n",
                static_cast<unsigned long long>(withoutCanvas_),
                static_cast<unsigned long long>(withoutMatrix_),
                static_cast<unsigned long long>(unreadableArguments_),
                static_cast<unsigned long long>(withoutArea_),
                static_cast<unsigned long long>(withheldByBudget_),
                static_cast<unsigned long long>(unresolvedPositions_),
                static_cast<unsigned long long>(withoutSink_));
    if (!contextErrors_.empty()) {
        std::printf("gmse01_boot:   graf context errors:");
        for (const auto& [error, count] : contextErrors_) {
            std::printf(" %s=%llu", name(error), static_cast<unsigned long long>(count));
        }
        std::printf("\n");
    }
}

} // namespace sunbright::gcnport_boot
