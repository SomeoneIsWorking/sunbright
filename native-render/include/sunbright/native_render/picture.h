#pragma once

#include <sunbright/native_render/semantic_2d_types.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace sb::native_render {

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
    bool operator==(const PictureTexture&) const = default;
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

// Inputs owned by J2DPicture::draw, the immediate-mode picture entry point used by HUD effects.
// Unlike drawFullSet, this path receives its destination rectangle and orientation directly and
// uses the position matrix produced by J2DPane::makeMatrix. Width and height are narrowed to the
// signed 16-bit values the retail vertex emitter consumes.
struct DirectPictureLayout {
    std::int32_t width = 0;
    std::int32_t height = 0;
    bool mirrorHorizontal = false;
    bool mirrorVertical = false;
    bool transpose = false;
    Matrix3x4 transform{};
};

using PictureMesh = std::array<SemanticVertex, 6>;

[[nodiscard]] bool valid(const PictureCommand& picture) noexcept;
[[nodiscard]] bool valid(const PictureDraw& draw) noexcept;
[[nodiscard]] bool resolve_picture_layout(const PictureLayout& layout,
                                          std::array<Vec2, 4>& positions,
                                          std::array<Vec2, 4>& uv) noexcept;
[[nodiscard]] bool resolve_direct_picture_layout(const DirectPictureLayout& layout,
                                                 std::array<Vec2, 4>& positions,
                                                 std::array<Vec2, 4>& uv) noexcept;
[[nodiscard]] PictureMesh make_mesh(const PictureCommand& picture) noexcept;

// Reference for the PC-native picture material. It blends decoded texture layers, applies the
// picture's black/white colour remap, then the interpolated corner tint and pane opacity. The GPU
// shader implements this same semantic formula rather than reproducing a TEV-stage program.
[[nodiscard]] Color shade(const PictureMaterial& material,
                          const std::array<Color, 4>& textureSamples, Color corner,
                          float opacity) noexcept;

} // namespace sb::native_render
