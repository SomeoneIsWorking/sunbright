#pragma once

#include <sunbright/title_adapter/guest_j2d_pane.h>
#include <sunbright/title_adapter/guest_j2d_primitives.h>
#include <sunbright/title_adapter/guest_jut_texture.h>
#include <sunbright/title_adapter/guest_memory.h>

#include <array>
#include <cstdint>

namespace sb::title_adapter {

// Reads one of GMSE01's `J2DPicture` panes, and the textures it draws with, out of guest memory.
//
// The title screen's whole visible composite is J2D: the logo, the fly-in letters, the shine,
// "PRESS START!" and the copyright line are each one textured quad drawn by a pane, and none of
// them is a `J3DShape`. A run that hooks only the model path sees the sky and the sea and nothing
// that sits on top of them -- a gap in what is observed, not in what the title draws.
//
// Nothing here resolves a layout, decodes an image or blends a colour.
// `native_render::resolve_picture_layout` already owns the crop, binding, mirror and wrap contract
// that turns a pane's bounds and transform into four corners, and `decode_image_rgba8` owns the
// tiled formats; the decomp adapter reaches both from host objects. This reaches the same functions
// from guest memory, so the two runtimes cannot disagree about what a pane means.

inline constexpr std::size_t GUEST_MAX_PICTURE_TEXTURES = 4;

// `J2DPicture`, which extends `J2DPane`.
inline constexpr GuestAddress GUEST_PICTURE_TEXTURES = 0xEC;
inline constexpr GuestAddress GUEST_PICTURE_TEXTURE_COUNT = 0xFC;
inline constexpr GuestAddress GUEST_PICTURE_BINDING = 0x128;
inline constexpr GuestAddress GUEST_PICTURE_MIRROR = 0x12C;
inline constexpr GuestAddress GUEST_PICTURE_FLIP = 0x130;
inline constexpr GuestAddress GUEST_PICTURE_WRAP_HORIZONTAL = 0x134;
inline constexpr GuestAddress GUEST_PICTURE_WRAP_VERTICAL = 0x138;
inline constexpr GuestAddress GUEST_PICTURE_WHITE = 0x13C;
inline constexpr GuestAddress GUEST_PICTURE_BLACK = 0x140;
inline constexpr GuestAddress GUEST_PICTURE_CORNER_COLORS = 0x144;
inline constexpr GuestAddress GUEST_PICTURE_BLEND_KONST_COLOR = 0x154;
inline constexpr GuestAddress GUEST_PICTURE_BLEND_KONST_ALPHA = 0x158;

enum class GuestPictureError : std::uint8_t {
    None,
    NoReader,
    NullPicture,
    UnreadablePicture,
    TextureCountOutOfRange,
    NullTexture,
    UnreadableTexture,
    UnreadablePalette,
    NullPaletteColorTable,
    EmptyBounds,
};

[[nodiscard]] const char* name(GuestPictureError error) noexcept;

struct GuestPicture {
    GuestAddress address = 0;
    GuestRect bounds{};
    GuestRect clipRect{};
    // Row-major 3x4, in the order `Mtx` stores it, so a caller copies rather than transposes.
    std::array<float, 12> positionMatrix{};
    std::array<float, 12> globalMatrix{};
    std::uint8_t colorAlpha = 0;
    std::uint8_t textureCount = 0;
    std::array<GuestJutTexture, GUEST_MAX_PICTURE_TEXTURES> textures{};
    std::uint32_t binding = 0;
    std::uint32_t mirror = 0;
    bool flip = false;
    std::int32_t wrapHorizontal = 0;
    std::int32_t wrapVertical = 0;
    std::uint32_t white = 0xFFFFFFFF;
    std::uint32_t black = 0;
    std::array<std::uint32_t, 4> cornerColors{};
    std::uint32_t blendKonstColor = 0;
    std::uint32_t blendKonstAlpha = 0;
};

[[nodiscard]] GuestPictureError read_guest_picture(const GuestMemory& memory, GuestAddress picture,
                                                   GuestPicture& out) noexcept;

} // namespace sb::title_adapter
