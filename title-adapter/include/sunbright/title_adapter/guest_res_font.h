#pragma once

#include <sunbright/title_adapter/guest_memory.h>

#include <array>
#include <cstdint>

namespace sb::title_adapter {

// Reads the glyph GMSE01's `JUTResFont` selects for one character code.
//
// A glyph is not a field anywhere. `JUTResFont::drawChar_scale` (0x802f1b00) calls `loadFont`,
// which walks the font resource's `WID1` blocks for the character's width entry and its `GLY1`
// blocks for the page and cell its image sits in, and only then emits the quad. That is why the
// decomp's own capture seam sits at the end of the body: by then the selection has happened.
//
// A guest hook cannot enter at the end, so this does the selection instead, from the same blocks
// the guest walks. Everything it needs is reachable from `this` at the function's entry, so nothing
// of the body has to run and the title is left untouched.

// `JUTFont`, the base.
inline constexpr GuestAddress GUEST_FONT_FIXED = 0x04;
inline constexpr GuestAddress GUEST_FONT_FIXED_WIDTH = 0x08;
// `mColor1`..`mColor4`, the four corner colours in the order the quad is emitted.
inline constexpr GuestAddress GUEST_FONT_COLORS = 0x0C;

// `JUTResFont`, which extends it. `mWidth` and `mHeight` are not the font's width and height: they
// are the selected cell's pixel offset into its glyph page, which `loadImage` writes.
inline constexpr GuestAddress GUEST_RES_FONT_CELL_X = 0x1C;
inline constexpr GuestAddress GUEST_RES_FONT_CELL_Y = 0x20;
inline constexpr GuestAddress GUEST_RES_FONT_PAGE_INDEX = 0x44;
inline constexpr GuestAddress GUEST_RES_FONT_INFO_BLOCK = 0x4C;
inline constexpr GuestAddress GUEST_RES_FONT_WIDTH_BLOCKS = 0x50;
inline constexpr GuestAddress GUEST_RES_FONT_GLYPH_BLOCKS = 0x54;
inline constexpr GuestAddress GUEST_RES_FONT_MAP_BLOCKS = 0x58;
inline constexpr GuestAddress GUEST_RES_FONT_WIDTH_BLOCK_COUNT = 0x5C;
inline constexpr GuestAddress GUEST_RES_FONT_GLYPH_BLOCK_COUNT = 0x5E;
inline constexpr GuestAddress GUEST_RES_FONT_MAP_BLOCK_COUNT = 0x60;
// `field_0x62`: which glyph block the loaded page came from.
inline constexpr GuestAddress GUEST_RES_FONT_GLYPH_BLOCK_INDEX = 0x62;
// `mMaxCode`, which despite its name is the *lowest* `MAP1` start code the font carries.
inline constexpr GuestAddress GUEST_RES_FONT_LOWEST_MAPPED_CODE = 0x64;

// `ResFONT::INF1`.
inline constexpr GuestAddress GUEST_FONT_INFO_TYPE = 0x08;
inline constexpr GuestAddress GUEST_FONT_INFO_ASCENT = 0x0A;
inline constexpr GuestAddress GUEST_FONT_INFO_DESCENT = 0x0C;
inline constexpr GuestAddress GUEST_FONT_INFO_WIDTH = 0x0E;
inline constexpr GuestAddress GUEST_FONT_INFO_LEADING = 0x10;
inline constexpr GuestAddress GUEST_FONT_INFO_DEFAULT_CODE = 0x12;

// `ResFONT::WID1`: a start and end code, then one two-byte entry per code in that range.
inline constexpr GuestAddress GUEST_FONT_WIDTH_START_CODE = 0x08;
inline constexpr GuestAddress GUEST_FONT_WIDTH_END_CODE = 0x0A;
inline constexpr GuestAddress GUEST_FONT_WIDTH_ENTRIES = 0x0C;

// `ResFONT::MAP1`.
inline constexpr GuestAddress GUEST_FONT_MAP_METHOD = 0x08;
inline constexpr GuestAddress GUEST_FONT_MAP_START_CODE = 0x0A;
inline constexpr GuestAddress GUEST_FONT_MAP_END_CODE = 0x0C;
inline constexpr GuestAddress GUEST_FONT_MAP_ENTRY_COUNT = 0x0E;
inline constexpr GuestAddress GUEST_FONT_MAP_ENTRIES = 0x10;

// `ResFONT::GLY1`.
inline constexpr GuestAddress GUEST_FONT_GLYPH_START_CODE = 0x08;
inline constexpr GuestAddress GUEST_FONT_GLYPH_END_CODE = 0x0A;
inline constexpr GuestAddress GUEST_FONT_GLYPH_CELL_WIDTH = 0x0C;
inline constexpr GuestAddress GUEST_FONT_GLYPH_CELL_HEIGHT = 0x0E;
inline constexpr GuestAddress GUEST_FONT_GLYPH_TEXTURE_SIZE = 0x10;
inline constexpr GuestAddress GUEST_FONT_GLYPH_TEXTURE_FORMAT = 0x14;
inline constexpr GuestAddress GUEST_FONT_GLYPH_ROWS = 0x16;
inline constexpr GuestAddress GUEST_FONT_GLYPH_COLUMNS = 0x18;
inline constexpr GuestAddress GUEST_FONT_GLYPH_TEXTURE_WIDTH = 0x1A;
inline constexpr GuestAddress GUEST_FONT_GLYPH_TEXTURE_HEIGHT = 0x1C;
inline constexpr GuestAddress GUEST_FONT_GLYPH_DATA = 0x20;

enum class GuestResFontError : std::uint8_t {
    None,
    NoReader,
    NullFont,
    UnreadableFont,
    NullInfoBlock,
    UnreadableInfoBlock,
    UnreadableBlockTable,
    UnreadableBlock,
    // The font states a zero width, height or cell grid, all of which the glyph arithmetic divides
    // by. A font that cannot place a cell is not a font with small glyphs.
    DegenerateFontMetrics,
    DegenerateGlyphPage,
};

[[nodiscard]] const char* name(GuestResFontError error) noexcept;

// How the character code was turned into a font code, reported rather than assumed: a US font that
// never leaves the direct method and a Japanese one that resolves through a table are the same
// reader taking different branches, and only the count says which happened.
enum class GuestFontMapping : std::uint8_t {
    // No `MAP1` block covers the code, so the font's own default code stands in.
    DefaultCode,
    Direct,     // method 0: code - startCode
    ShiftJis,   // method 1
    Table,      // method 2: one entry per code
    SortedPairs // method 3: binary search over (code, index) pairs
};

[[nodiscard]] const char* name(GuestFontMapping mapping) noexcept;

// The page a glyph's image sits in, and where in it.
struct GuestGlyphPage {
    GuestAddress block = 0;
    // The page's own first byte: `&block->data[pageIndex * textureSize]`.
    GuestAddress data = 0;
    std::uint32_t textureFormat = 0;
    std::uint32_t textureSize = 0;
    std::uint16_t textureWidth = 0;
    std::uint16_t textureHeight = 0;
    std::uint16_t cellWidth = 0;
    std::uint16_t cellHeight = 0;
    std::uint16_t pageIndex = 0;
    std::uint16_t blockIndex = 0;
    // True when no `GLY1` block covers the code. `loadImage` then returns without touching the
    // loaded page, so the guest draws whichever page is already bound -- which is what this
    // reports, read from the font's own `mTexPageIdx`/`field_0x62`, rather than a page it picked.
    bool retained = false;
};

struct GuestResFontGlyph {
    GuestAddress font = 0;
    std::uint32_t code = 0;
    std::uint32_t fontCode = 0;
    GuestFontMapping mapping = GuestFontMapping::DefaultCode;
    std::uint16_t fontWidth = 0;
    std::uint16_t ascent = 0;
    std::uint16_t descent = 0;
    std::uint16_t leading = 0;
    std::uint16_t fontType = 0;
    // The `WID1` entry: the glyph's left bearing and its own width. Absent from every block means
    // `{0, fontWidth}`, which is what `loadFont` substitutes.
    std::uint8_t leftBearing = 0;
    std::uint8_t glyphWidth = 0;
    bool hasWidthEntry = false;
    std::uint32_t fixedWidth = 0;
    bool fixed = false;
    // The cell's pixel offset into its page.
    std::uint32_t cellX = 0;
    std::uint32_t cellY = 0;
    std::array<std::uint32_t, 4> corner{};
    GuestGlyphPage page{};
};

[[nodiscard]] GuestResFontError read_guest_res_font_glyph(const GuestMemory& memory,
                                                          GuestAddress font, std::uint32_t code,
                                                          GuestResFontGlyph& out) noexcept;

} // namespace sb::title_adapter
