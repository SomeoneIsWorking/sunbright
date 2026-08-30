#pragma once

#include "guest_byte_reader.h"

#include <sunbright/native_render/glyph.h>
#include <sunbright/native_render/image.h>

#include <cstdint>
#include <vector>

namespace sb::recomp {

struct ResourceGlyphArgs {
    float positionX = 0.0f;
    float positionY = 0.0f;
    float scaleX = 0.0f;
    float scaleY = 0.0f;
    std::uint32_t code = 0;
    bool applyBearing = false;
};

struct CapturedGlyph {
    native_render::GlyphCommand command{};
    std::vector<std::uint8_t> rgba8{};
    native_render::DecodedImageView image{};

    void refresh_image_view() noexcept;
};

// Capture after the retained drawChar_scale body: loadFont has then selected the exact glyph block,
// page, cell, and width entry that the retail code used. The adapter decodes that resource page and
// publishes only font/layout semantics, never GX texture objects or TEV state.
[[nodiscard]] bool capture_resource_font_glyph(const GuestByteReader& byteReader,
                                               std::uint32_t self, const ResourceGlyphArgs& args,
                                               const native_render::Matrix3x4& transform,
                                               native_render::Color black,
                                               native_render::Color white,
                                               CapturedGlyph& capture) noexcept;

} // namespace sb::recomp
