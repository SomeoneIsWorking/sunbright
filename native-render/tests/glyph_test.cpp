#include <sunbright/native_render/glyph.h>

#include <cassert>
#include <cmath>

namespace {

constexpr sb::native_render::Matrix3x4 kIdentity{
    {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}};

bool near(float first, float second) {
    return std::fabs(first - second) < 0.00001f;
}

} // namespace

int main() {
    using namespace sb::native_render;

    ResourceGlyphLayout layout{.positionX = 100.0f,
                               .positionY = 80.0f,
                               .scaleX = 16.0f,
                               .scaleY = 24.0f,
                               .fontWidth = 16,
                               .fontHeight = 24,
                               .ascent = 18,
                               .descent = 6,
                               .leftBearing = 3,
                               .glyphWidth = 11,
                               .fixedWidth = 20,
                               .fixed = false,
                               .applyBearing = true,
                               .cellX = 17,
                               .cellY = 25,
                               .atlasWidth = 128,
                               .atlasHeight = 96,
                               .transform = kIdentity};
    ResolvedGlyphLayout resolved{};
    assert(resolve_resource_glyph_layout(layout, resolved));
    assert(resolved.positions[0] == Vec2(97.0f, 62.0f));
    assert(resolved.positions[3] == Vec2(113.0f, 86.0f));
    assert(near(resolved.advance, 11.0f));

    // The UV result retains JUTResFont's integer division before normalization. A direct floating
    // division differs for this deliberately non-divisible atlas cell and would fail this control.
    const auto fixed = [](std::uint32_t value, std::uint32_t extent) {
        return static_cast<float>((static_cast<std::uint64_t>(value) * 0x8000U) / extent) /
               32768.0f;
    };
    assert(near(resolved.uv[0].x, fixed(17, 128)));
    assert(near(resolved.uv[3].y, fixed(49, 96)));

    // Disabling bearing changes both the origin and the advance, proving those two retail branches
    // are wired independently rather than hidden behind one guessed width.
    layout.applyBearing = false;
    assert(resolve_resource_glyph_layout(layout, resolved));
    assert(resolved.positions[0] == Vec2(100.0f, 62.0f));
    assert(near(resolved.advance, 14.0f));

    layout.fixed = true;
    layout.applyBearing = true;
    assert(resolve_resource_glyph_layout(layout, resolved));
    assert(resolved.positions[0] == Vec2(100.0f, 62.0f));
    assert(near(resolved.advance, 20.0f));

    GlyphCommand glyph{
        .instance = 0x1234,
        .code = 'A',
        .positions = resolved.positions,
        .uv = resolved.uv,
        .corner = {Color{1, 0, 0, 1}, Color{0, 1, 0, 1}, Color{0, 0, 1, 1}, Color{1, 1, 1, 1}},
        .atlas = {.resource = 0x5678, .width = 128, .height = 96, .hasAlpha = true},
        .black = {0, 0, 0, 0},
        .white = {1, 1, 1, 1}};
    assert(valid(glyph));
    const PictureCommand picture = picture_from_glyph(glyph);
    assert(picture.material.textureCount == 1);
    assert(picture.material.textures[0] == glyph.atlas);
    assert(picture.positions == glyph.positions && picture.uv == glyph.uv);
    assert(make_mesh(glyph) == make_mesh(picture));

    layout.atlasWidth = 0;
    assert(!resolve_resource_glyph_layout(layout, resolved));
    glyph.atlas.resource = 0;
    assert(!valid(glyph));
    return 0;
}
