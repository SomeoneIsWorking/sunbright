#include <sunbright/native_render/picture.h>

#include <array>
#include <cassert>
#include <cmath>

namespace {

using sb::native_render::Canvas;
using sb::native_render::Color;
using sb::native_render::DirectPictureLayout;
using sb::native_render::PictureCommand;
using sb::native_render::PictureLayout;
using sb::native_render::PictureMaterial;
using sb::native_render::PictureTexture;
using sb::native_render::Vec2;

bool close(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001f;
}

void require_color(Color actual, Color expected) {
    assert(close(actual.r, expected.r));
    assert(close(actual.g, expected.g));
    assert(close(actual.b, expected.b));
    assert(close(actual.a, expected.a));
}

} // namespace

int main() {
    sb::native_render::AddressMode address{};
    assert(sb::native_render::decode_address_mode(2, address));
    assert(address == sb::native_render::AddressMode::Mirror);
    assert(!sb::native_render::decode_address_mode(3, address));
    sb::native_render::FilterMode filter{};
    sb::native_render::MipFilter mip{};
    assert(sb::native_render::decode_min_filter(5, filter, mip));
    assert(filter == sb::native_render::FilterMode::Linear);
    assert(mip == sb::native_render::MipFilter::Linear);
    assert(!sb::native_render::decode_min_filter(6, filter, mip));
    assert(sb::native_render::decode_mag_filter(1, filter));
    assert(filter == sb::native_render::FilterMode::Linear);
    assert(!sb::native_render::decode_mag_filter(2, filter));
    float blend = 0.0f;
    assert(sb::native_render::decode_blend_factor(0x00336699, 2, blend));
    assert(close(blend, 0x66 / 255.0f));
    assert(!sb::native_render::decode_blend_factor(0x00336699, 0, blend));

    PictureCommand picture{};
    picture.instance = 0x81234567;
    picture.positions = {Vec2{10.0f, 20.0f}, Vec2{110.0f, 20.0f}, Vec2{10.0f, 70.0f},
                         Vec2{110.0f, 70.0f}};
    picture.uv = {Vec2{0.25f, 0.0f}, Vec2{0.75f, 0.0f}, Vec2{0.25f, 1.0f}, Vec2{0.75f, 1.0f}};
    picture.corner = {Color{1.0f, 0.0f, 0.0f, 1.0f}, Color{0.0f, 1.0f, 0.0f, 1.0f},
                      Color{0.0f, 0.0f, 1.0f, 1.0f}, Color{1.0f, 1.0f, 1.0f, 0.5f}};
    picture.opacity = 0.5f;
    picture.clip = {.enabled = true, .x = 0, .y = 0, .width = 640, .height = 480};

    PictureMaterial material{};
    material.textureCount = 2;
    material.textures[0] =
        PictureTexture{.resource = 0x1000, .width = 32, .height = 16, .hasAlpha = true};
    material.textures[1] =
        PictureTexture{.resource = 0x2000, .width = 32, .height = 16, .hasAlpha = false};
    material.textures[1].colorMix = 0.25f;
    material.textures[1].alphaMix = 0.75f;
    material.black = Color{0.1f, 0.2f, 0.3f, 0.4f};
    material.white = Color{0.9f, 0.8f, 0.7f, 1.0f};
    picture.material = material;

    assert(sb::native_render::valid(picture));
    const auto mesh = sb::native_render::make_mesh(picture);
    constexpr std::array<unsigned, 6> expectedCorners{0, 1, 3, 0, 3, 2};
    for (std::size_t i = 0; i < mesh.size(); ++i) {
        const unsigned corner = expectedCorners[i];
        assert(mesh[i].position == picture.positions[corner]);
        assert(mesh[i].uv == picture.uv[corner]);
        assert(mesh[i].color == picture.corner[corner]);
    }

    // The PC material owns semantic layer blending directly. Layer 1 has no alpha, so its sampled
    // alpha is one before alphaMix; black/white remap then applies before the vertex tint/opacity.
    const std::array<Color, 4> samples{Color{0.2f, 0.4f, 0.6f, 0.5f}, Color{1.0f, 0.0f, 0.5f, 0.1f},
                                       Color{}, Color{}};
    require_color(sb::native_render::shade(material, samples, Color{0.5f, 1.0f, 0.25f, 0.8f}, 0.5f),
                  Color{0.21f, 0.38f, 0.1325f, 0.37f});

    PictureCommand missingTexture = picture;
    missingTexture.material.textures[0].resource = 0;
    assert(!sb::native_render::valid(missingTexture));

    PictureCommand tooManyTextures = picture;
    tooManyTextures.material.textureCount = 5;
    assert(!sb::native_render::valid(tooManyTextures));

    PictureCommand degenerate = picture;
    degenerate.positions[3] = degenerate.positions[0];
    degenerate.positions[1] = degenerate.positions[0];
    degenerate.positions[2] = degenerate.positions[0];
    assert(!sb::native_render::valid(degenerate));

    PictureCommand unclipped = picture;
    unclipped.clip = {};
    assert(sb::native_render::valid(unclipped));

    // Spec-derived from J2DPicture::drawFullSet: a 64x32 texture centered in a 100x50 pane
    // trims the quad to [18,9..82,41]. Parent × global transform then translates it by (13,24).
    PictureLayout layout{};
    layout.width = 100;
    layout.height = 50;
    layout.textureWidth = 64;
    layout.textureHeight = 32;
    layout.parentTransform.value = {1, 0, 0, 3, 0, 1, 0, 4, 0, 0, 1, 0};
    layout.globalTransform.value = {1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 0};
    std::array<Vec2, 4> positions{};
    std::array<Vec2, 4> uv{};
    assert(sb::native_render::resolve_picture_layout(layout, positions, uv));
    assert(
        (positions == std::array<Vec2, 4>{Vec2{31, 33}, Vec2{95, 33}, Vec2{31, 65}, Vec2{95, 65}}));
    assert((uv == std::array<Vec2, 4>{Vec2{0, 0}, Vec2{1, 0}, Vec2{0, 1}, Vec2{1, 1}}));

    // Deliberately different mirror+transpose control. This must change UV association without
    // changing the transformed coverage.
    PictureLayout transposed = layout;
    transposed.textureWidth = 50;
    transposed.textureHeight = 100;
    transposed.transpose = true;
    transposed.horizontalWrap = 1;
    transposed.verticalWrap = 1;
    transposed.binding = 0x0f;
    transposed.mirror = 0x03;
    std::array<Vec2, 4> transposedPositions{};
    std::array<Vec2, 4> transposedUv{};
    assert(
        sb::native_render::resolve_picture_layout(transposed, transposedPositions, transposedUv));
    assert(transposedPositions != positions);
    assert((transposedUv == std::array<Vec2, 4>{Vec2{1, 0}, Vec2{1, 1}, Vec2{0, 0}, Vec2{0, 1}}));

    // Spec-derived from J2DPicture::draw: makeMatrix has already produced this translated transform
    // and the immediate emitter writes local (0,0)..(w,h) vertices with horizontally mirrored UVs.
    DirectPictureLayout direct{};
    direct.width = 48;
    direct.height = 20;
    direct.mirrorHorizontal = true;
    direct.transform.value = {1, 0, 0, 12, 0, 1, 0, 34, 0, 0, 1, 0};
    std::array<Vec2, 4> directPositions{};
    std::array<Vec2, 4> directUv{};
    assert(sb::native_render::resolve_direct_picture_layout(direct, directPositions, directUv));
    assert((directPositions ==
            std::array<Vec2, 4>{Vec2{12, 34}, Vec2{60, 34}, Vec2{12, 54}, Vec2{60, 54}}));
    assert((directUv == std::array<Vec2, 4>{Vec2{1, 0}, Vec2{0, 0}, Vec2{1, 1}, Vec2{0, 1}}));

    // Known-different control for the transposed retail branch. U now varies vertically and V
    // horizontally; this must not collapse to the ordinary mirror mapping above.
    direct.transpose = true;
    direct.mirrorVertical = true;
    std::array<Vec2, 4> directTransposedUv{};
    assert(sb::native_render::resolve_direct_picture_layout(direct, directPositions,
                                                            directTransposedUv));
    assert((directTransposedUv ==
            std::array<Vec2, 4>{Vec2{1, 0}, Vec2{1, 1}, Vec2{0, 0}, Vec2{0, 1}}));
    assert(directTransposedUv != directUv);

    // The guest emitter narrows dimensions to s16. Pin both the narrowing and its degenerate
    // refusal so the semantic path cannot quietly use a different-sized quad.
    direct.transpose = false;
    direct.width = 65536;
    assert(!sb::native_render::resolve_direct_picture_layout(direct, directPositions, directUv));

    sb::native_render::PixelRect scissor{};
    const Canvas canvas{.origin = {100, 50}, .extent = {200, 100}, .viewport = {50, 25, 200, 100}};
    assert(sb::native_render::valid(canvas));
    assert(sb::native_render::resolve_scissor(canvas, {}, 400, 200, scissor));
    assert((scissor == sb::native_render::PixelRect{50, 25, 200, 100}));
    assert(sb::native_render::resolve_scissor(
        canvas, {.enabled = true, .x = 90, .y = 75, .width = 60, .height = 50}, 400, 200, scissor));
    assert((scissor == sb::native_render::PixelRect{40, 50, 60, 50}));
    assert(!sb::native_render::resolve_scissor(
        canvas, {.enabled = true, .x = 400, .y = 400, .width = 10, .height = 10}, 400, 200,
        scissor));
}
