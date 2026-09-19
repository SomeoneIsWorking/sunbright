// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_glyph_probe.h"

#include <sunbright/native_render/semantic_sink.h>

#include <array>
#include <cstdio>
#include <span>

namespace sunbright::gcnport_boot {
namespace {

constexpr std::size_t FIRST_ARGUMENT_REGISTER = 3;
constexpr std::size_t SECOND_ARGUMENT_REGISTER = 4;
constexpr std::size_t THIRD_ARGUMENT_REGISTER = 5;
constexpr std::size_t POSITION_X_REGISTER = 1;
constexpr std::size_t POSITION_Y_REGISTER = 2;
constexpr std::size_t SCALE_X_REGISTER = 3;
constexpr std::size_t SCALE_Y_REGISTER = 4;

bool read_through_guest_context(sb::title_adapter::GuestAddress address,
                                std::span<std::uint8_t> destination, void* context) {
    auto* const guest = static_cast<gcnport::GuestContext*>(context);
    return guest->read_memory(address, std::as_writable_bytes(destination));
}

// `JUtility::TColor` is a four-byte aggregate, which CodeWarrior passes by address: the argument
// register holds a pointer to the colour and not the colour itself.
bool read_color_argument(gcnport::GuestContext& guest, std::size_t argumentRegister,
                         std::uint32_t& color) {
    const auto address =
        static_cast<sb::title_adapter::GuestAddress>(guest.general_register(argumentRegister));
    const sb::title_adapter::GuestMemory memory{read_through_guest_context, &guest};
    const sb::title_adapter::GuestReader reader(memory);
    return reader.word(address, color);
}

} // namespace

const char* GuestGlyphProbe::entry_name(Entry entry) noexcept {
    switch (entry) {
    case Entry::SetGXDefault:
        return "setgx";
    case Entry::SetGXRemap:
        return "remap";
    case Entry::DrawChar:
        return "glyph";
    }
    return "unknown";
}

bool GuestGlyphProbe::resolve_page(gcnport::GuestContext& guest,
                                   const sb::title_adapter::GuestGlyphPage& page,
                                   const DecodedPage*& decoded) {
    const PageKey key{page.data, page.textureFormat,
                      (static_cast<std::uint32_t>(page.textureWidth) << 16U) | page.textureHeight};
    if (const auto found = pageCache_.find(key); found != pageCache_.end()) {
        decoded = &found->second;
        return true;
    }

    sb::native_render::EncodedImageFormat format{};
    std::size_t sourceBytes = 0;
    std::size_t outputBytes = 0;
    if (page.textureFormat > 0xFFU ||
        !sb::native_render::decode_image_format(static_cast<std::uint8_t>(page.textureFormat),
                                                format) ||
        !sb::native_render::encoded_image_data_size(page.textureWidth, page.textureHeight, format,
                                                    sourceBytes) ||
        sourceBytes > page.textureSize ||
        !sb::native_render::decoded_image_data_size(page.textureWidth, page.textureHeight,
                                                    outputBytes)) {
        pageErrors_[sb::native_render::ImageDecodeError::UnsupportedFormat] += 1;
        return false;
    }

    encodedBytes_.assign(sourceBytes, 0);
    if (!guest.read_memory(page.data, std::as_writable_bytes(std::span(encodedBytes_)))) {
        pageErrors_[sb::native_render::ImageDecodeError::SourceTooShort] += 1;
        return false;
    }
    const sb::native_render::EncodedImageView encoded{format, page.textureWidth, page.textureHeight,
                                                      encodedBytes_};

    DecodedPage value{};
    value.rgba8.assign(outputBytes, 0);
    const sb::native_render::ImageDecodeError error =
        sb::native_render::decode_image_rgba8(encoded, value.rgba8);
    pageErrors_[error] += 1;
    if (error != sb::native_render::ImageDecodeError::None) {
        return false;
    }
    if (!sb::native_render::image_content_revision(encoded, value.revision)) {
        pageErrors_[sb::native_render::ImageDecodeError::UnsupportedFormat] += 1;
        return false;
    }
    // A glyph page carries no palette, so the palette format is never consulted; it is named here
    // because the query takes one and not because a font could be colour-indexed.
    value.hasAlpha = sb::native_render::encoded_image_format_has_alpha(
        format, sb::native_render::PaletteFormat::Rgb5A3);
    pagesDecoded_ += 1;
    pageBytes_ += value.rgba8.size();
    pageFormats_[format] += 1;
    decoded = &pageCache_.emplace(key, std::move(value)).first->second;
    return true;
}

gcnport::HookResult GuestGlyphProbe::record_remap(gcnport::GuestContext& guest) {
    const auto font = static_cast<sb::title_adapter::GuestAddress>(
        guest.general_register(FIRST_ARGUMENT_REGISTER));
    if (remaps_ == nullptr) {
        return gcnport::HookResult::call_original_once();
    }
    if (entry_ == Entry::SetGXDefault) {
        // The no-argument overload programs the ramp's two ends as they come: black transparent,
        // white opaque. It is the same state the two-argument one dispatches here to produce.
        (*remaps_)[font] = Remap{};
        return gcnport::HookResult::call_original_once();
    }

    std::uint32_t black = 0;
    std::uint32_t white = 0;
    if (!read_color_argument(guest, SECOND_ARGUMENT_REGISTER, black) ||
        !read_color_argument(guest, THIRD_ARGUMENT_REGISTER, white)) {
        return gcnport::HookResult::call_original_once();
    }
    (*remaps_)[font] = Remap{sb::native_render::color_from_rgba8(black),
                             sb::native_render::color_from_rgba8(white)};
    if (reports_ < maxReports_) {
        reports_ += 1;
        std::printf("gmse01_boot: font 0x%08x maps its ramp onto 0x%08x..0x%08x\n", font, black,
                    white);
    }
    return gcnport::HookResult::call_original_once();
}

gcnport::HookResult GuestGlyphProbe::publish_glyph(gcnport::GuestContext& guest) {
    const auto font = static_cast<sb::title_adapter::GuestAddress>(
        guest.general_register(FIRST_ARGUMENT_REGISTER));
    const auto code = static_cast<std::uint32_t>(guest.general_register(SECOND_ARGUMENT_REGISTER));
    const bool applyBearing = guest.general_register(THIRD_ARGUMENT_REGISTER) != 0;

    const sb::title_adapter::GuestMemory memory{read_through_guest_context, &guest};
    sb::title_adapter::GuestResFontGlyph glyph{};
    const sb::title_adapter::GuestResFontError error =
        sb::title_adapter::read_guest_res_font_glyph(memory, font, code, glyph);
    glyphErrors_[error] += 1;
    if (error != sb::title_adapter::GuestResFontError::None) {
        return gcnport::HookResult::call_original_once();
    }
    mappings_[glyph.mapping] += 1;
    if (glyph.page.retained) {
        retainedPages_ += 1;
    }

    const sb::native_render::Canvas* const canvas =
        screenSpace_ != nullptr ? screenSpace_->current() : nullptr;
    if (canvas == nullptr) {
        withoutCanvas_ += 1;
        return gcnport::HookResult::call_original_once();
    }
    const sb::native_render::Matrix3x4* const transform =
        matrices_ != nullptr ? matrices_->current() : nullptr;
    if (transform == nullptr) {
        withoutMatrix_ += 1;
        return gcnport::HookResult::call_original_once();
    }
    if (remaps_ == nullptr) {
        withoutRemap_ += 1;
        return gcnport::HookResult::call_original_once();
    }
    const auto remap = remaps_->find(font);
    if (remap == remaps_->end()) {
        // The title has not said what this font's intensity ramp means yet. Publishing under a
        // guessed pair would draw legible text in the wrong colour, which is harder to see than
        // text that is missing.
        withoutRemap_ += 1;
        return gcnport::HookResult::call_original_once();
    }

    const DecodedPage* page = nullptr;
    if (!resolve_page(guest, glyph.page, page)) {
        undecodablePage_ += 1;
        return gcnport::HookResult::call_original_once();
    }

    if (budget_ != nullptr && !budget_->take()) {
        withheldByBudget_ += 1;
        return gcnport::HookResult::call_original_once();
    }

    sb::native_render::ResourceGlyphLayout layout{
        .positionX = static_cast<float>(guest.floating_register(POSITION_X_REGISTER)),
        .positionY = static_cast<float>(guest.floating_register(POSITION_Y_REGISTER)),
        .scaleX = static_cast<float>(guest.floating_register(SCALE_X_REGISTER)),
        .scaleY = static_cast<float>(guest.floating_register(SCALE_Y_REGISTER)),
        .fontWidth = glyph.fontWidth,
        .fontHeight = static_cast<std::uint32_t>(glyph.ascent) + glyph.descent,
        .ascent = glyph.ascent,
        .descent = glyph.descent,
        .leftBearing = glyph.leftBearing,
        .glyphWidth = glyph.glyphWidth,
        .fixedWidth = glyph.fixedWidth,
        .fixed = glyph.fixed,
        .applyBearing = applyBearing,
        .cellX = glyph.cellX,
        .cellY = glyph.cellY,
        .atlasWidth = glyph.page.textureWidth,
        .atlasHeight = glyph.page.textureHeight,
        .transform = *transform};
    sb::native_render::ResolvedGlyphLayout resolved{};
    if (!sb::native_render::resolve_resource_glyph_layout(layout, resolved)) {
        unresolvedLayout_ += 1;
        if (refusalReports_ < MAX_REFUSAL_REPORTS) {
            refusalReports_ += 1;
            std::printf("gmse01_boot: glyph 0x%08x code 0x%x has no layout: at (%g, %g) scale "
                        "%gx%g, font %ux%u, cell %u,%u of a %ux%u page\n",
                        font, code, static_cast<double>(layout.positionX),
                        static_cast<double>(layout.positionY), static_cast<double>(layout.scaleX),
                        static_cast<double>(layout.scaleY), layout.fontWidth, layout.fontHeight,
                        layout.cellX, layout.cellY, layout.atlasWidth, layout.atlasHeight);
        }
        return gcnport::HookResult::call_original_once();
    }

    sb::native_render::GlyphCommand command{};
    command.instance = font;
    command.code = code;
    command.positions = resolved.positions;
    command.uv = resolved.uv;
    command.atlas = {.resource = glyph.page.data,
                     .revision = page->revision,
                     .width = glyph.page.textureWidth,
                     .height = glyph.page.textureHeight,
                     .minFilter = sb::native_render::FilterMode::Linear,
                     .magFilter = sb::native_render::FilterMode::Linear,
                     .hasAlpha = page->hasAlpha};
    for (std::size_t corner = 0; corner < command.corner.size(); ++corner) {
        command.corner[corner] = sb::native_render::color_from_rgba8(glyph.corner[corner]);
    }
    command.black = remap->second.black;
    command.white = remap->second.white;

    if (reports_ < maxReports_) {
        reports_ += 1;
        std::printf("gmse01_boot: glyph 0x%08x code 0x%x -> font code 0x%x (%s) cell %u,%u of "
                    "%ux%u page %u at (%g, %g) scale %gx%g\n",
                    font, code, glyph.fontCode, name(glyph.mapping), glyph.cellX, glyph.cellY,
                    glyph.page.textureWidth, glyph.page.textureHeight, glyph.page.pageIndex,
                    static_cast<double>(layout.positionX), static_cast<double>(layout.positionY),
                    static_cast<double>(layout.scaleX), static_cast<double>(layout.scaleY));
    }

    if (!sb::native_render::has_semantic_sink()) {
        withoutSink_ += 1;
        return gcnport::HookResult::call_original_once();
    }
    submitted_ += 1;
    const sb::native_render::DecodedImageView image{command.atlas.resource, command.atlas.revision,
                                                    command.atlas.width, command.atlas.height,
                                                    page->rgba8};
    const sb::native_render::GlyphDraw draw{*canvas, command};
    if (sb::native_render::submit_glyph(draw, std::span(&image, 1))) {
        acceptedBySink_ += 1;
    }
    return gcnport::HookResult::call_original_once();
}

gcnport::HookResult GuestGlyphProbe::operator()(gcnport::GuestContext& guest) {
    entries_ += 1;
    if (entry_ != Entry::DrawChar) {
        return record_remap(guest);
    }
    return publish_glyph(guest);
}

void GuestGlyphProbe::report() const {
    if (entry_ != Entry::DrawChar) {
        std::printf("gmse01_boot: guest glyph probe (%s): %llu entr(ies), %llu font(s) with a "
                    "stated ramp\n",
                    entry_name(entry_), static_cast<unsigned long long>(entries_),
                    static_cast<unsigned long long>(remaps_ != nullptr ? remaps_->size() : 0));
        return;
    }
    std::printf("gmse01_boot: guest glyph probe: %llu character(s) drawn, %llu submitted, %llu "
                "accepted by the sink\n",
                static_cast<unsigned long long>(entries_),
                static_cast<unsigned long long>(submitted_),
                static_cast<unsigned long long>(acceptedBySink_));
    std::printf("gmse01_boot:   glyph errors:");
    for (const auto& [error, count] : glyphErrors_) {
        std::printf(" %s=%llu", name(error), static_cast<unsigned long long>(count));
    }
    std::printf("\n");
    std::printf("gmse01_boot:   not published: no_canvas=%llu no_matrix=%llu no_remap=%llu "
                "undecodable_page=%llu withheld_by_budget=%llu unresolved_layout=%llu "
                "no_sink=%llu\n",
                static_cast<unsigned long long>(withoutCanvas_),
                static_cast<unsigned long long>(withoutMatrix_),
                static_cast<unsigned long long>(withoutRemap_),
                static_cast<unsigned long long>(undecodablePage_),
                static_cast<unsigned long long>(withheldByBudget_),
                static_cast<unsigned long long>(unresolvedLayout_),
                static_cast<unsigned long long>(withoutSink_));
    if (!mappings_.empty()) {
        std::printf("gmse01_boot:   code mappings:");
        for (const auto& [mapping, count] : mappings_) {
            std::printf(" %s=%llu", name(mapping), static_cast<unsigned long long>(count));
        }
        std::printf("\n");
    }
    std::printf("gmse01_boot:   glyph pages: %llu decoded, %llu distinct, %llu byte(s); %llu "
                "character(s) drew with the page already bound\n",
                static_cast<unsigned long long>(pagesDecoded_),
                static_cast<unsigned long long>(pageCache_.size()),
                static_cast<unsigned long long>(pageBytes_),
                static_cast<unsigned long long>(retainedPages_));
    if (!pageFormats_.empty()) {
        std::printf("gmse01_boot:   page formats:");
        for (const auto& [format, count] : pageFormats_) {
            std::printf(" %s=%llu", sb::native_render::encoded_image_format_name(format),
                        static_cast<unsigned long long>(count));
        }
        std::printf("\n");
    }
    if (!pageErrors_.empty()) {
        std::printf("gmse01_boot:   page decode results:");
        for (const auto& [error, count] : pageErrors_) {
            std::printf(" %s=%llu", sb::native_render::image_decode_error_name(error),
                        static_cast<unsigned long long>(count));
        }
        std::printf("\n");
    }
}

} // namespace sunbright::gcnport_boot
