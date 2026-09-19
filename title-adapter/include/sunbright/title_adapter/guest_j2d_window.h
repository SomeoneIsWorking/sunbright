#pragma once

#include <sunbright/title_adapter/guest_j2d_primitives.h>
#include <sunbright/title_adapter/guest_jut_texture.h>
#include <sunbright/title_adapter/guest_memory.h>

#include <array>
#include <cstdint>

namespace sb::title_adapter {

// Reads one of GMSE01's `J2DWindow` panes -- the framed panel almost every piece of the title's
// text is written onto -- out of guest memory.
//
// A window is not one quad. `draw_private` fills a four-corner gradient, optionally stretches a
// contents texture over it, and then, only when all four corner textures are present, lays a frame
// out of those four textures mirrored into up to eight pieces. `native_render::resolve_window_
// layout` already owns that arithmetic for the decomp-side adapter; this reads the state it needs
// from the guest's own object so the two runtimes cannot disagree about what a window means.
//
// The outer rectangle, the contents rectangle and the parent transform are `draw_private`'s
// arguments rather than fields, so they are read at the call site from its pointers; a window's
// own fields are everything here.

// `J2DWindow`, which extends `J2DPane`.
inline constexpr GuestAddress GUEST_WINDOW_CONTENTS_BOUNDS = 0xEC;
inline constexpr GuestAddress GUEST_WINDOW_FRAME_TEXTURES = 0x100;
inline constexpr GuestAddress GUEST_WINDOW_CONTENTS_TEXTURE = 0x110;
inline constexpr GuestAddress GUEST_WINDOW_MIRROR = 0x114;
inline constexpr GuestAddress GUEST_WINDOW_CONTENTS_COLORS = 0x118;
inline constexpr GuestAddress GUEST_WINDOW_FRAME_WHITE = 0x128;
inline constexpr GuestAddress GUEST_WINDOW_FRAME_BLACK = 0x12C;
inline constexpr GuestAddress GUEST_WINDOW_MINIMUM_WIDTH = 0x130;
inline constexpr GuestAddress GUEST_WINDOW_MINIMUM_HEIGHT = 0x134;

inline constexpr std::size_t GUEST_WINDOW_FRAME_TEXTURE_COUNT = 4;
inline constexpr std::size_t GUEST_WINDOW_CONTENTS_COLOR_COUNT = 4;

enum class GuestWindowError : std::uint8_t {
    None,
    NoReader,
    NullWindow,
    UnreadableWindow,
    UnreadableTexture,
    UnreadablePalette,
    NullPaletteColorTable,
};

[[nodiscard]] const char* name(GuestWindowError error) noexcept;

struct GuestWindow {
    GuestAddress address = 0;
    GuestRect clipRect{};
    // Row-major 3x4, in the order `Mtx` stores it, so a caller copies rather than transposes.
    std::array<float, 12> globalMatrix{};
    std::uint8_t colorAlpha = 0;
    std::int32_t minimumWidth = 0;
    std::int32_t minimumHeight = 0;
    std::uint32_t mirror = 0;
    // Retail draws a frame only when all four corner textures are present -- one missing corner is
    // a window with no frame at all, not a window with three corners. `hasFrame` is that `if`, and
    // `frameTextures` is read only when it holds.
    bool hasFrame = false;
    std::array<GuestJutTexture, GUEST_WINDOW_FRAME_TEXTURE_COUNT> frameTextures{};
    bool hasContentsTexture = false;
    GuestJutTexture contentsTexture{};
    // Top-left, top-right, bottom-left, bottom-right, in the order the object stores them.
    std::array<std::uint32_t, GUEST_WINDOW_CONTENTS_COLOR_COUNT> contentsColors{};
    std::uint32_t frameWhite = 0xFFFFFFFF;
    std::uint32_t frameBlack = 0;
};

[[nodiscard]] GuestWindowError read_guest_window(const GuestMemory& memory, GuestAddress window,
                                                 GuestWindow& out) noexcept;

} // namespace sb::title_adapter
