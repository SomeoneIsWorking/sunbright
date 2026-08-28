#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sb::native_render {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
    bool operator==(const Vec2&) const = default;
};

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
    bool operator==(const Color&) const = default;
};

struct ClipRect {
    bool enabled = false;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct PixelRect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool operator==(const PixelRect&) const = default;
};

// One J2D screen's logical coordinate system and its physical placement inside the native target.
// A game frame may contain several distinct canvases, so this value travels with each draw.
struct Canvas {
    Vec2 origin{};
    Vec2 extent{};
    PixelRect viewport{};
    bool operator==(const Canvas&) const = default;
};

enum class AddressMode : std::uint8_t { Clamp, Repeat, Mirror };
enum class FilterMode : std::uint8_t { Nearest, Linear };
enum class MipFilter : std::uint8_t { None, Nearest, Linear };

[[nodiscard]] bool decode_address_mode(std::uint8_t raw, AddressMode& mode) noexcept;
[[nodiscard]] bool decode_min_filter(std::uint8_t raw, FilterMode& filter, MipFilter& mip) noexcept;
[[nodiscard]] bool decode_mag_filter(std::uint8_t raw, FilterMode& filter) noexcept;
[[nodiscard]] bool decode_blend_factor(std::uint32_t packed, std::size_t textureIndex,
                                       float& factor) noexcept;

struct PictureTexture {
    std::uint64_t resource = 0;
    // Zero identifies immutable content. Mutable image owners increment this whenever the decoded
    // RGBA payload changes so a backend cannot silently reuse a stale upload.
    std::uint64_t revision = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    AddressMode addressU = AddressMode::Clamp;
    AddressMode addressV = AddressMode::Clamp;
    FilterMode minFilter = FilterMode::Nearest;
    FilterMode magFilter = FilterMode::Nearest;
    MipFilter mipFilter = MipFilter::None;
    bool hasAlpha = false;
    float colorMix = 1.0f;
    float alphaMix = 1.0f;
};

struct PictureMaterial {
    std::array<PictureTexture, 4> textures{};
    std::uint8_t textureCount = 0;
    Color black{};
    Color white{1.0f, 1.0f, 1.0f, 1.0f};
};

// Final game-semantic J2D picture values. Positions are in the game's logical screen space after
// its pane/model transform; no GX registers, TEV program, FIFO offset, or EFB operation crosses
// this boundary. Corner order is top-left, top-right, bottom-left, bottom-right.
struct PictureCommand {
    std::uint64_t instance = 0;
    std::array<Vec2, 4> positions{};
    std::array<Vec2, 4> uv{};
    std::array<Color, 4> corner{Color{1.0f, 1.0f, 1.0f, 1.0f}, Color{1.0f, 1.0f, 1.0f, 1.0f},
                                Color{1.0f, 1.0f, 1.0f, 1.0f}, Color{1.0f, 1.0f, 1.0f, 1.0f}};
    float opacity = 1.0f;
    ClipRect clip{};
    PictureMaterial material{};
};

struct PictureDraw {
    Canvas canvas{};
    PictureCommand picture{};
};

struct Matrix3x4 {
    std::array<float, 12> value{};
};

// Inputs owned by the game's J2D layout code. This is shared by the decomp and recomp adapters;
// it reproduces J2DPicture::drawFullSet's crop/binding/mirror/transpose contract without exposing
// GX calls or registers to the renderer.
struct PictureLayout {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::uint32_t textureWidth = 0;
    std::uint32_t textureHeight = 0;
    std::uint32_t binding = 0;
    std::uint32_t mirror = 0;
    bool transpose = false;
    std::int32_t horizontalWrap = 0;
    std::int32_t verticalWrap = 0;
    Matrix3x4 parentTransform{};
    Matrix3x4 globalTransform{};
};

struct PictureVertex {
    Vec2 position{};
    Vec2 uv{};
    Color color{};
};

using PictureMesh = std::array<PictureVertex, 6>;

[[nodiscard]] bool valid(const PictureCommand& picture) noexcept;
[[nodiscard]] bool valid(const Canvas& canvas) noexcept;
[[nodiscard]] bool valid(const PictureDraw& draw) noexcept;
[[nodiscard]] bool resolve_picture_layout(const PictureLayout& layout,
                                          std::array<Vec2, 4>& positions,
                                          std::array<Vec2, 4>& uv) noexcept;
[[nodiscard]] bool resolve_scissor(const Canvas& canvas, const ClipRect& clip,
                                   std::uint32_t targetWidth, std::uint32_t targetHeight,
                                   PixelRect& scissor) noexcept;
[[nodiscard]] PictureMesh make_mesh(const PictureCommand& picture) noexcept;

// Reference for the PC-native picture material. It blends decoded texture layers, applies the
// picture's black/white colour remap, then the interpolated corner tint and pane opacity. The GPU
// shader implements this same semantic formula rather than reproducing a TEV-stage program.
[[nodiscard]] Color shade(const PictureMaterial& material,
                          const std::array<Color, 4>& textureSamples, Color corner,
                          float opacity) noexcept;

} // namespace sb::native_render
