#pragma once

#include <sunbright/native_render/picture.h>

#include <cstdint>

namespace sb::native_render {

// One resource-font glyph after JUTResFont has selected its glyph page and width entry. It remains
// a distinct semantic family for coverage and ordering, while sharing the renderer's decoded-image
// material implementation with J2D pictures.
struct GlyphCommand {
    std::uint64_t instance = 0;
    std::uint32_t code = 0;
    std::array<Vec2, 4> positions{};
    std::array<Vec2, 4> uv{};
    std::array<Color, 4> corner{};
    ClipRect clip{};
    PictureTexture atlas{};
    Color black{};
    Color white{1.0f, 1.0f, 1.0f, 1.0f};
};

struct GlyphDraw {
    Canvas canvas{};
    GlyphCommand glyph{};
};

// Values selected by JUTResFont::drawChar_scale and loadFont. The fixed-point atlas conversion is
// deliberately resolved here so recomp and decomp adapters use the same retail arithmetic.
struct ResourceGlyphLayout {
    float positionX = 0.0f;
    float positionY = 0.0f;
    float scaleX = 0.0f;
    float scaleY = 0.0f;
    std::uint32_t fontWidth = 0;
    std::uint32_t fontHeight = 0;
    std::uint32_t ascent = 0;
    std::uint32_t descent = 0;
    std::uint32_t leftBearing = 0;
    std::uint32_t glyphWidth = 0;
    std::uint32_t fixedWidth = 0;
    bool fixed = false;
    bool applyBearing = false;
    std::uint32_t cellX = 0;
    std::uint32_t cellY = 0;
    std::uint32_t atlasWidth = 0;
    std::uint32_t atlasHeight = 0;
    Matrix3x4 transform{};
};

struct ResolvedGlyphLayout {
    std::array<Vec2, 4> positions{};
    std::array<Vec2, 4> uv{};
    float advance = 0.0f;
};

[[nodiscard]] bool resolve_resource_glyph_layout(const ResourceGlyphLayout& layout,
                                                 ResolvedGlyphLayout& resolved) noexcept;
[[nodiscard]] PictureCommand picture_from_glyph(const GlyphCommand& glyph) noexcept;
[[nodiscard]] bool valid(const GlyphCommand& glyph) noexcept;
[[nodiscard]] bool valid(const GlyphDraw& draw) noexcept;
[[nodiscard]] PictureMesh make_mesh(const GlyphCommand& glyph) noexcept;

} // namespace sb::native_render
