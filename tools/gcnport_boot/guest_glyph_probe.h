// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include <sunbright/native_render/glyph.h>
#include <sunbright/native_render/image_decode.h>
#include <sunbright/title_adapter/guest_res_font.h>

#include "frame_draw_budget.h"
#include "gcnport/guest_context.h"
#include "gcnport/native_hooks.h"
#include "guest_matrix_state.h"
#include "guest_screen_space.h"

namespace sunbright::gcnport_boot {

// Publishes the last producer of GMSE01's 2D stream: the glyphs a `JUTResFont` draws.
//
// Every menu, the HUD, the file-select cards and every line of dialogue are resource-font glyphs.
// None is a `J2DPicture` and none is a `J3DShape`: `JUTResFont::drawChar_scale` selects a cell of a
// glyph page and pushes four vertices itself, so a renderer fed only by the pane and model probes
// draws the boxes around the text and none of the text.
//
// It sits on three entries of one class. `drawChar_scale` (0x802f1b00) is the draw; the two `setGX`
// overloads (0x802f178c and 0x802f1864) are how the title states what the font's intensity ramp
// maps onto, which is a colour pair per font and not per glyph. Entering `drawChar_scale` is
// deliberate: `loadFont` has not run yet, so nothing of the selection is readable from the object,
// and `title_adapter::read_guest_res_font_glyph` performs that same selection from the font
// resource's own blocks instead of waiting for the body to leave its result behind.
class GuestGlyphProbe {
  public:
    enum class Entry : std::uint8_t { SetGXDefault, SetGXRemap, DrawChar };

    // `screen_space` and `matrices` may be null, which is a run that asked for glyphs without
    // asking for the state that places them; every glyph is then counted as unplaceable rather
    // than published somewhere guessed. `budget` may be null, which is an unbounded run.
    GuestGlyphProbe(Entry entry, const GuestScreenSpace* screen_space,
                    const GuestMatrixState* matrices, FrameDrawBudget* budget,
                    std::uint64_t max_reports) noexcept
        : entry_(entry), screenSpace_(screen_space), matrices_(matrices), budget_(budget),
          maxReports_(max_reports) {}

    gcnport::HookResult operator()(gcnport::GuestContext& guest);

    void report() const;

    [[nodiscard]] static const char* entry_name(Entry entry) noexcept;

    // The three entries share one publisher's state, so the run installs one of these per entry and
    // points them at the same store.
    struct Remap {
        sb::native_render::Color black{};
        sb::native_render::Color white{1.0F, 1.0F, 1.0F, 1.0F};
    };
    using RemapStore = std::map<sb::title_adapter::GuestAddress, Remap>;

    void share(RemapStore* remaps) noexcept { remaps_ = remaps; }

  private:
    // One decoded glyph page, kept so a font drawn every frame decodes once. The key is the page's
    // first byte together with its extent and format: a font that switches pages mid-string changes
    // it, and one whose bytes are rewritten in place does not, which is what `revision` covers.
    struct PageKey {
        std::uint32_t data = 0;
        std::uint32_t format = 0;
        std::uint32_t extent = 0;
        auto operator<=>(const PageKey&) const = default;
    };

    struct DecodedPage {
        std::vector<std::uint8_t> rgba8;
        std::uint64_t revision = 0;
        bool hasAlpha = false;
    };

    gcnport::HookResult record_remap(gcnport::GuestContext& guest);
    gcnport::HookResult publish_glyph(gcnport::GuestContext& guest);
    [[nodiscard]] bool resolve_page(gcnport::GuestContext& guest,
                                    const sb::title_adapter::GuestGlyphPage& page,
                                    const DecodedPage*& decoded);

    Entry entry_ = Entry::DrawChar;
    const GuestScreenSpace* screenSpace_ = nullptr;
    const GuestMatrixState* matrices_ = nullptr;
    FrameDrawBudget* budget_ = nullptr;
    RemapStore* remaps_ = nullptr;
    std::uint64_t maxReports_ = 0;
    std::uint64_t reports_ = 0;

    std::uint64_t entries_ = 0;
    std::uint64_t withoutCanvas_ = 0;
    std::uint64_t withoutMatrix_ = 0;
    std::uint64_t withoutRemap_ = 0;
    std::uint64_t undecodablePage_ = 0;
    std::uint64_t withheldByBudget_ = 0;
    std::uint64_t unresolvedLayout_ = 0;
    // A layout refusal always prints, on its own small budget, whatever the reporting budget is.
    // "One glyph in 230,784 was refused" is not a finding; the scale, position and page it was
    // refused for is, and the refusal is exactly the case nobody would have thought to ask about.
    static constexpr std::uint64_t MAX_REFUSAL_REPORTS = 4;
    std::uint64_t refusalReports_ = 0;
    std::uint64_t withoutSink_ = 0;
    std::uint64_t submitted_ = 0;
    std::uint64_t acceptedBySink_ = 0;
    std::uint64_t pagesDecoded_ = 0;
    std::uint64_t pageBytes_ = 0;
    std::uint64_t retainedPages_ = 0;
    std::map<sb::title_adapter::GuestResFontError, std::uint64_t> glyphErrors_;
    std::map<sb::title_adapter::GuestFontMapping, std::uint64_t> mappings_;
    std::map<sb::native_render::ImageDecodeError, std::uint64_t> pageErrors_;
    // What the pages a run decoded were encoded as. A font page's intensity is its coverage, so
    // which format it is decides whether its alpha means anything -- a fact worth printing rather
    // than inferring from a glyph that came out as a block.
    std::map<sb::native_render::EncodedImageFormat, std::uint64_t> pageFormats_;
    std::map<PageKey, DecodedPage> pageCache_;
    std::vector<std::uint8_t> encodedBytes_;
};

} // namespace sunbright::gcnport_boot
