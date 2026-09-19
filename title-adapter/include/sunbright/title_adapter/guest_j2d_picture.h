#pragma once

#include <sunbright/title_adapter/guest_j2d_rect.h>
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

// `J2DPane`.
inline constexpr GuestAddress GUEST_PANE_BOUNDS = 0x14;
inline constexpr GuestAddress GUEST_PANE_CLIP_RECT = 0x34;
inline constexpr GuestAddress GUEST_PANE_POSITION_MATRIX = 0x54;
inline constexpr GuestAddress GUEST_PANE_GLOBAL_MATRIX = 0x84;
inline constexpr GuestAddress GUEST_PANE_COLOR_ALPHA = 0xCD;

// `J2DPicture`, which extends it.
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

// `JUTTexture`. The encoded image is read through these and not through the `ResTIMG` at 0x20:
// `storeTIMG` can repoint the object at another resource's pixels, and `JUTTexture` is what
// `load` hands GX. The resource address is carried out all the same, as the identity a decoded
// image is cached under.
inline constexpr GuestAddress GUEST_JUT_TEXTURE_RESOURCE = 0x20;
inline constexpr GuestAddress GUEST_JUT_TEXTURE_DATA = 0x24;
inline constexpr GuestAddress GUEST_JUT_TEXTURE_ACTIVE_PALETTE = 0x2C;
inline constexpr GuestAddress GUEST_JUT_TEXTURE_FORMAT = 0x34;
inline constexpr GuestAddress GUEST_JUT_TEXTURE_ALPHA_ENABLED = 0x38;
inline constexpr GuestAddress GUEST_JUT_TEXTURE_WIDTH = 0x3C;
inline constexpr GuestAddress GUEST_JUT_TEXTURE_HEIGHT = 0x3E;
inline constexpr GuestAddress GUEST_JUT_TEXTURE_WRAP_S = 0x40;
inline constexpr GuestAddress GUEST_JUT_TEXTURE_WRAP_T = 0x41;
inline constexpr GuestAddress GUEST_JUT_TEXTURE_MIN_FILTER = 0x42;
inline constexpr GuestAddress GUEST_JUT_TEXTURE_MAG_FILTER = 0x43;

// `JUTPalette`, past the 12-byte `GXTlutObj` it opens with.
inline constexpr GuestAddress GUEST_JUT_PALETTE_FORMAT = 0x10;
inline constexpr GuestAddress GUEST_JUT_PALETTE_COLOR_TABLE = 0x14;
inline constexpr GuestAddress GUEST_JUT_PALETTE_ENTRIES = 0x18;

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

// The colour table one indexed texture samples through. Read whenever the texture names one,
// because whether a format is indexed is the image decoder's question and not this reader's.
struct GuestJutPalette {
    GuestAddress address = 0;
    GuestAddress colorTable = 0;
    std::uint32_t format = 0;
    std::uint16_t entries = 0;

    [[nodiscard]] bool present() const noexcept { return address != 0; }
};

struct GuestJutTexture {
    GuestAddress address = 0;
    GuestAddress resource = 0;
    GuestAddress data = 0;
    std::uint32_t format = 0;
    std::uint32_t alphaEnabled = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t wrapS = 0;
    std::uint8_t wrapT = 0;
    std::uint8_t minFilter = 0;
    std::uint8_t magFilter = 0;
    GuestJutPalette palette{};
};

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

// Reads one `Mtx` -- the parent transform `J2DPane::draw` hands `drawSelf`, which lives on the
// guest stack rather than in any object this can reach from the pane.
[[nodiscard]] bool read_guest_matrix(const GuestMemory& memory, GuestAddress matrix,
                                     std::array<float, 12>& out) noexcept;

} // namespace sb::title_adapter
