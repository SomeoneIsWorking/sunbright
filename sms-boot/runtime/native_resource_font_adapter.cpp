#include "host_allocation_scope.h"
#include "native_j2d_context.h"

#include <sunbright/native_render/glyph.h>
#include <sunbright/native_render/image_decode.h>
#include <sunbright/native_render/semantic_sink.h>

#include <sb_native_j2d.h>

#include <JSystem/JUtility/JUTRect.hpp>
#include <dolphin/mtx.h>
#include <dolphin/os.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

struct NativeTextContext {
    sb::native_render::Canvas canvas{};
    sb::native_render::ClipRect clip{};
    sb::native_render::Matrix3x4 transform{};
};

std::array<NativeTextContext, 32> g_textContexts{};
std::array<bool, 32> g_textContextHasValue{};
std::size_t g_textContextDepth = 0;
sb::native_render::Color g_fontBlack{};
sb::native_render::Color g_fontWhite{1, 1, 1, 1};
bool g_fontRemapKnown = false;

[[noreturn]] void fail(const char* reason, const void* font, std::uint32_t code) {
    OSPanic(__FILE__, __LINE__, "semantic resource-font glyph capture failed: %s font=%p code=%08x",
            reason, font, code);
}

bool format_has_alpha(sb::native_render::EncodedImageFormat format) noexcept {
    return format != sb::native_render::EncodedImageFormat::Rgb565;
}

} // namespace

extern "C" void sb_native_text_context_push(const void* clipRectPointer,
                                            const void* transformPointer) {
    if (g_textContextDepth == g_textContexts.size())
        fail("text context stack overflow", clipRectPointer, 0);
    bool captured = false;
    if (sb::native_render::has_semantic_sink()) {
        if (clipRectPointer == nullptr || transformPointer == nullptr)
            fail("null text context", clipRectPointer, 0);
        const sb::native_render::PictureContext* pictureContext = sb::current_native_j2d_context();
        if (pictureContext == nullptr)
            fail("missing or non-orthographic J2D canvas", clipRectPointer, 0);

        const auto& clipRect = *static_cast<const JUTRect*>(clipRectPointer);
        const auto& matrix = *static_cast<const Mtx*>(transformPointer);
        NativeTextContext context{.canvas = pictureContext->canvas};
        for (std::size_t index = 0; index < context.transform.value.size(); ++index)
            context.transform.value[index] = (&matrix[0][0])[index];
        if (!sb::native_render::valid(context.transform))
            fail("invalid text transform", clipRectPointer, 0);
        if (pictureContext->clipEnabled) {
            if (clipRect.getWidth() <= 0 || clipRect.getHeight() <= 0)
                fail("invalid text clip", clipRectPointer, 0);
            context.clip = {.enabled = true,
                            .x = clipRect.x1,
                            .y = clipRect.y1,
                            .width = static_cast<std::uint32_t>(clipRect.getWidth()),
                            .height = static_cast<std::uint32_t>(clipRect.getHeight())};
        }
        g_textContexts[g_textContextDepth] = context;
        captured = true;
    }
    g_textContextHasValue[g_textContextDepth++] = captured;
}

extern "C" void sb_native_text_context_pop(void) {
    if (g_textContextDepth == 0)
        fail("text context stack underflow", nullptr, 0);
    --g_textContextDepth;
}

extern "C" void sb_native_font_remap(unsigned int black, unsigned int white) {
    if (!sb::native_render::has_semantic_sink()) {
        g_fontRemapKnown = false;
        return;
    }
    g_fontBlack = sb::native_render::color_from_rgba8(black);
    g_fontWhite = sb::native_render::color_from_rgba8(white);
    g_fontRemapKnown = true;
}

extern "C" void sb_native_font_glyph_submit(const SbNativeFontGlyph* source) {
    if (!sb::native_render::has_semantic_sink())
        return;
    if (source == nullptr || source->instance == nullptr || source->atlas == nullptr)
        fail("null glyph input", source, 0);
    const void* fontPointer = source->instance;
    const std::uint32_t code = source->code;
    if (g_textContextDepth == 0 || !g_textContextHasValue[g_textContextDepth - 1U])
        fail("missing high-level J2DTextBox context", fontPointer, code);
    if (!g_fontRemapKnown)
        fail("missing preceding JUTResFont::setGX remap", fontPointer, code);

    if (source->atlas_size == 0 || source->atlas_width == 0 || source->atlas_height == 0 ||
        source->atlas_format > std::numeric_limits<std::uint8_t>::max()) {
        fail("invalid selected glyph block", fontPointer, code);
    }

    sb::native_render::EncodedImageFormat format{};
    std::size_t sourceBytes = 0;
    std::size_t outputBytes = 0;
    if (!sb::native_render::decode_image_format(static_cast<std::uint8_t>(source->atlas_format),
                                                format) ||
        format == sb::native_render::EncodedImageFormat::Indexed4 ||
        format == sb::native_render::EncodedImageFormat::Indexed8 ||
        format == sb::native_render::EncodedImageFormat::Indexed14 ||
        !sb::native_render::encoded_image_data_size(source->atlas_width, source->atlas_height,
                                                    format, sourceBytes) ||
        sourceBytes > source->atlas_size ||
        !sb::native_render::decoded_image_data_size(source->atlas_width, source->atlas_height,
                                                    outputBytes)) {
        fail("unsupported glyph-page encoding or extent", fontPointer, code);
    }
    const auto* page = static_cast<const std::uint8_t*>(source->atlas);
    const sb::native_render::EncodedImageView encoded{
        format, source->atlas_width, source->atlas_height, {page, sourceBytes}};

    const sb::HostAllocationScope hostAllocations;
    std::vector<std::uint8_t> rgba8(outputBytes);
    if (sb::native_render::decode_image_rgba8(encoded, rgba8) !=
        sb::native_render::ImageDecodeError::None) {
        fail("glyph-page decode failed", fontPointer, code);
    }

    sb::native_render::ResourceGlyphLayout layout{
        .positionX = source->position_x,
        .positionY = source->position_y,
        .scaleX = source->scale_x,
        .scaleY = source->scale_y,
        .fontWidth = source->font_width,
        .fontHeight = source->font_height,
        .ascent = source->ascent,
        .descent = source->descent,
        .leftBearing = source->left_bearing,
        .glyphWidth = source->glyph_width,
        .fixedWidth = source->fixed_width,
        .fixed = source->fixed != 0,
        .applyBearing = source->apply_bearing != 0,
        .cellX = source->cell_x,
        .cellY = source->cell_y,
        .atlasWidth = source->atlas_width,
        .atlasHeight = source->atlas_height,
        .transform = g_textContexts[g_textContextDepth - 1U].transform};
    sb::native_render::ResolvedGlyphLayout resolved{};
    if (!sb::native_render::resolve_resource_glyph_layout(layout, resolved))
        fail("glyph layout resolution refused the selected state", fontPointer, code);

    sb::native_render::GlyphCommand command{};
    command.instance = reinterpret_cast<std::uintptr_t>(source->instance);
    command.code = code;
    command.positions = resolved.positions;
    command.uv = resolved.uv;
    command.clip = g_textContexts[g_textContextDepth - 1U].clip;
    command.atlas = {.resource = reinterpret_cast<std::uintptr_t>(page),
                     .width = source->atlas_width,
                     .height = source->atlas_height,
                     .minFilter = sb::native_render::FilterMode::Linear,
                     .magFilter = sb::native_render::FilterMode::Linear,
                     .hasAlpha = format_has_alpha(format)};
    if (!sb::native_render::image_content_revision(encoded, command.atlas.revision))
        fail("glyph-page revision failed", fontPointer, code);
    command.corner = {sb::native_render::color_from_rgba8(source->corner[0]),
                      sb::native_render::color_from_rgba8(source->corner[1]),
                      sb::native_render::color_from_rgba8(source->corner[2]),
                      sb::native_render::color_from_rgba8(source->corner[3])};
    command.black = g_fontBlack;
    command.white = g_fontWhite;
    const sb::native_render::DecodedImageView image{command.atlas.resource, command.atlas.revision,
                                                    command.atlas.width, command.atlas.height,
                                                    rgba8};
    const sb::native_render::GlyphDraw draw{g_textContexts[g_textContextDepth - 1U].canvas,
                                            command};
    if (!sb::native_render::submit_glyph(draw, std::span(&image, 1)))
        fail("sink rejected a validated glyph", fontPointer, code);
}
