#include <sunbright/native_render/glyph.h>

#include <cmath>
#include <limits>

namespace sb::native_render {
namespace {

bool fixed_uv(std::uint32_t cell, std::uint32_t extent, std::uint32_t atlasExtent, float& first,
              float& second) noexcept {
    if (atlasExtent == 0 || cell > std::numeric_limits<std::uint32_t>::max() - extent)
        return false;
    const std::uint64_t firstFixed = (static_cast<std::uint64_t>(cell) * 0x8000U) / atlasExtent;
    const std::uint64_t secondFixed =
        (static_cast<std::uint64_t>(cell + extent) * 0x8000U) / atlasExtent;
    constexpr float kFixedScale = 1.0f / 32768.0f;
    first = static_cast<float>(firstFixed) * kFixedScale;
    second = static_cast<float>(secondFixed) * kFixedScale;
    return true;
}

} // namespace

bool resolve_resource_glyph_layout(const ResourceGlyphLayout& layout,
                                   ResolvedGlyphLayout& resolved) noexcept {
    if (!std::isfinite(layout.positionX) || !std::isfinite(layout.positionY) ||
        !std::isfinite(layout.scaleX) || !std::isfinite(layout.scaleY) || layout.scaleX == 0.0f ||
        layout.scaleY == 0.0f || layout.fontWidth == 0 || layout.fontHeight == 0 ||
        layout.atlasWidth == 0 || layout.atlasHeight == 0 || !valid(layout.transform)) {
        return false;
    }

    const float widthScale = layout.scaleX / static_cast<float>(layout.fontWidth);
    const float heightScale = layout.scaleY / static_cast<float>(layout.fontHeight);
    const float x1 = layout.fixed || !layout.applyBearing
                         ? layout.positionX
                         : layout.positionX - static_cast<float>(layout.leftBearing) * widthScale;
    const float x2 = x1 + layout.scaleX;
    const float y1 = layout.positionY - static_cast<float>(layout.ascent) * heightScale;
    const float y2 = layout.positionY + static_cast<float>(layout.descent) * heightScale;

    float u1 = 0.0f;
    float u2 = 0.0f;
    float v1 = 0.0f;
    float v2 = 0.0f;
    if (!fixed_uv(layout.cellX, layout.fontWidth, layout.atlasWidth, u1, u2) ||
        !fixed_uv(layout.cellY, layout.fontHeight, layout.atlasHeight, v1, v2)) {
        return false;
    }

    resolved.positions = {
        transform_point(layout.transform, x1, y1), transform_point(layout.transform, x2, y1),
        transform_point(layout.transform, x1, y2), transform_point(layout.transform, x2, y2)};
    resolved.uv = {Vec2{u1, v1}, Vec2{u2, v1}, Vec2{u1, v2}, Vec2{u2, v2}};
    const std::uint32_t advanceWidth =
        layout.fixed ? layout.fixedWidth
                     : layout.glyphWidth + (layout.applyBearing ? 0U : layout.leftBearing);
    resolved.advance = static_cast<float>(advanceWidth) * widthScale;
    return std::isfinite(resolved.advance);
}

PictureCommand picture_from_glyph(const GlyphCommand& glyph) noexcept {
    PictureMaterial material{};
    material.textureCount = 1;
    material.textures[0] = glyph.atlas;
    material.black = glyph.black;
    material.white = glyph.white;
    return {glyph.instance, glyph.positions, glyph.uv, glyph.corner, 1.0f, glyph.clip, material};
}

bool valid(const GlyphCommand& glyph) noexcept {
    return valid(picture_from_glyph(glyph));
}

bool valid(const GlyphDraw& draw) noexcept {
    return valid(draw.canvas) && valid(draw.glyph);
}

PictureMesh make_mesh(const GlyphCommand& glyph) noexcept {
    return make_mesh(picture_from_glyph(glyph));
}

} // namespace sb::native_render
