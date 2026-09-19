// Drives the shipping resource-font reader over a synthetic font resource.
//
// The reader's whole job is to reproduce a selection the guest makes across three functions, so the
// cases below are about the selection and not about reading fields: which map block claims a code,
// which cell of which page the code lands in, and what happens when no block claims it at all. The
// glyph grid is deliberately not square, because `loadImage` divides the cell index by the block's
// row count and not by its column count -- an asymmetry a square grid would hide.

#include <sunbright/title_adapter/guest_res_font.h>

#include "guest_image.h"

#include <cassert>
#include <cstdint>

namespace {

using sb::title_adapter::GuestAddress;
using sb::title_adapter::GuestFontMapping;
using sb::title_adapter::GuestMemory;
using sb::title_adapter::GuestResFontError;
using sb::title_adapter::GuestResFontGlyph;
using sb::title_adapter::read_guest_res_font_glyph;
using sb::title_adapter::test::Image;
using sb::title_adapter::test::read_image;

constexpr GuestAddress FONT = 0x80000100;
constexpr GuestAddress INFO = 0x80000200;
constexpr GuestAddress WIDTH_TABLE = 0x80000300;
constexpr GuestAddress GLYPH_TABLE = 0x80000310;
constexpr GuestAddress MAP_TABLE = 0x80000320;
constexpr GuestAddress WIDTH_BLOCK = 0x80000400;
constexpr GuestAddress MAP_BLOCK = 0x80000500;
constexpr GuestAddress GLYPH_BLOCK = 0x80000600;

constexpr std::uint16_t CELL_WIDTH = 12;
constexpr std::uint16_t CELL_HEIGHT = 16;
constexpr std::uint16_t ROWS = 2;
constexpr std::uint16_t COLUMNS = 3;
constexpr std::uint32_t TEXTURE_SIZE = 0x200;
constexpr std::uint16_t FONT_WIDTH = 10;
constexpr std::uint16_t DEFAULT_CODE = 0x30;

// A one-byte font whose map, width and glyph blocks all start at the space character: the shape a
// western `ResFONT` has, and the one every readable case below varies from.
Image western_font() {
    Image image;
    image.byte(FONT + 0x04, 0);  // not fixed-width
    image.word(FONT + 0x08, 14); // the fixed width it would use if it were
    image.word(FONT + 0x0C, 0x112233FF);
    image.word(FONT + 0x10, 0x445566FF);
    image.word(FONT + 0x14, 0x778899FF);
    image.word(FONT + 0x18, 0xAABBCCFF);
    image.word(FONT + 0x1C, 0); // mWidth: the loaded cell's x, which loadImage rewrites
    image.word(FONT + 0x20, 0); // mHeight
    image.word(FONT + 0x44, 0); // mTexPageIdx
    image.word(FONT + 0x4C, INFO);
    image.word(FONT + 0x50, WIDTH_TABLE);
    image.word(FONT + 0x54, GLYPH_TABLE);
    image.word(FONT + 0x58, MAP_TABLE);
    image.half(FONT + 0x5C, 1);
    image.half(FONT + 0x5E, 1);
    image.half(FONT + 0x60, 1);
    image.half(FONT + 0x62, 0);
    image.half(FONT + 0x64, 0x20); // mMaxCode: the lowest map start, below the two-byte range

    image.half(INFO + 0x08, 0); // fontType
    image.half(INFO + 0x0A, 12);
    image.half(INFO + 0x0C, 4);
    image.half(INFO + 0x0E, FONT_WIDTH);
    image.half(INFO + 0x10, 18);
    image.half(INFO + 0x12, DEFAULT_CODE);

    image.word(WIDTH_TABLE, WIDTH_BLOCK);
    image.word(GLYPH_TABLE, GLYPH_BLOCK);
    image.word(MAP_TABLE, MAP_BLOCK);

    image.half(MAP_BLOCK + 0x08, 0); // direct
    image.half(MAP_BLOCK + 0x0A, 0x20);
    image.half(MAP_BLOCK + 0x0C, 0x7E);
    image.half(MAP_BLOCK + 0x0E, 0x5F);

    image.half(WIDTH_BLOCK + 0x08, 0);
    image.half(WIDTH_BLOCK + 0x0A, 0x5E);
    // Font code 0x21 -- what 'A' maps to -- gets a bearing of 2 and a width of 9.
    image.byte(WIDTH_BLOCK + 0x0C + 0x21 * 2, 2);
    image.byte(WIDTH_BLOCK + 0x0C + 0x21 * 2 + 1, 9);

    image.half(GLYPH_BLOCK + 0x08, 0);
    image.half(GLYPH_BLOCK + 0x0A, 0x5E);
    image.half(GLYPH_BLOCK + 0x0C, CELL_WIDTH);
    image.half(GLYPH_BLOCK + 0x0E, CELL_HEIGHT);
    image.word(GLYPH_BLOCK + 0x10, TEXTURE_SIZE);
    image.half(GLYPH_BLOCK + 0x14, 4); // GX_TF_RGB565, stated rather than assumed
    image.half(GLYPH_BLOCK + 0x16, ROWS);
    image.half(GLYPH_BLOCK + 0x18, COLUMNS);
    image.half(GLYPH_BLOCK + 0x1A, 64);
    image.half(GLYPH_BLOCK + 0x1C, 64);
    return image;
}

void places_a_glyph_in_the_cell_the_guest_would_load() {
    Image image = western_font();
    GuestMemory memory{read_image, &image};

    GuestResFontGlyph glyph{};
    assert(read_guest_res_font_glyph(memory, FONT, 'A', glyph) == GuestResFontError::None);
    assert(glyph.font == FONT);
    assert(glyph.code == 'A');
    assert(glyph.mapping == GuestFontMapping::Direct);
    assert(glyph.fontCode == 0x21);

    // 0x21 in a six-cell page: page 5, cell 3 of it, which is row 1 column 1 under the guest's own
    // divide-by-rows.
    assert(glyph.page.pageIndex == 5);
    assert(glyph.cellX == CELL_WIDTH);
    assert(glyph.cellY == CELL_HEIGHT);
    assert(glyph.page.block == GLYPH_BLOCK);
    assert(glyph.page.blockIndex == 0);
    assert(glyph.page.data == GLYPH_BLOCK + 0x20 + 5 * TEXTURE_SIZE);
    assert(glyph.page.textureFormat == 4);
    assert(glyph.page.cellWidth == CELL_WIDTH && glyph.page.cellHeight == CELL_HEIGHT);
    assert(!glyph.page.retained);

    assert(glyph.hasWidthEntry);
    assert(glyph.leftBearing == 2 && glyph.glyphWidth == 9);
    assert(glyph.fontWidth == FONT_WIDTH);
    assert(glyph.ascent == 12 && glyph.descent == 4 && glyph.leading == 18);
    assert(!glyph.fixed && glyph.fixedWidth == 14);
    assert(glyph.corner[0] == 0x112233FF && glyph.corner[3] == 0xAABBCCFF);
}

// A code no `WID1` block covers is not an error: `loadFont` substitutes a zero bearing and the
// font's own width, and a reader that refused here would drop a glyph the console draws.
void substitutes_the_font_width_where_no_entry_exists() {
    Image image = western_font();
    image.half(WIDTH_BLOCK + 0x0A, 0x10); // the block now ends below every mapped code
    GuestMemory memory{read_image, &image};

    GuestResFontGlyph glyph{};
    assert(read_guest_res_font_glyph(memory, FONT, 'A', glyph) == GuestResFontError::None);
    assert(!glyph.hasWidthEntry);
    assert(glyph.leftBearing == 0);
    assert(glyph.glyphWidth == FONT_WIDTH);
}

// When no `GLY1` covers the code, `loadImage` returns without binding anything, so the guest draws
// the page and cell already loaded. Those are the font's own fields, and reporting a page this
// reader picked instead would name a texture the console never had bound.
void keeps_the_loaded_page_when_no_block_covers_the_code() {
    Image image = western_font();
    image.half(GLYPH_BLOCK + 0x0A, 0x10);
    image.word(FONT + 0x44, 3);  // mTexPageIdx
    image.word(FONT + 0x1C, 24); // mWidth: the cell already selected
    image.word(FONT + 0x20, 32); // mHeight
    GuestMemory memory{read_image, &image};

    GuestResFontGlyph glyph{};
    assert(read_guest_res_font_glyph(memory, FONT, 'A', glyph) == GuestResFontError::None);
    assert(glyph.page.retained);
    assert(glyph.page.pageIndex == 3);
    assert(glyph.page.data == GLYPH_BLOCK + 0x20 + 3 * TEXTURE_SIZE);
    assert(glyph.cellX == 24 && glyph.cellY == 32);
}

// No `MAP1` claims the code, so the font's default code stands in -- and it is the default code
// that is then looked up, not the character.
void falls_back_to_the_fonts_own_default_code() {
    Image image = western_font();
    GuestMemory memory{read_image, &image};

    GuestResFontGlyph glyph{};
    assert(read_guest_res_font_glyph(memory, FONT, 0x1F, glyph) == GuestResFontError::None);
    assert(glyph.mapping == GuestFontMapping::DefaultCode);
    assert(glyph.fontCode == DEFAULT_CODE);
}

void searches_the_sorted_pairs_a_table_font_carries() {
    Image image = western_font();
    image.half(MAP_BLOCK + 0x08, 3);
    image.half(MAP_BLOCK + 0x0E, 3);
    const std::uint16_t codes[3] = {0x30, 0x41, 0x5A};
    const std::uint16_t indices[3] = {7, 19, 33};
    for (std::uint16_t entry = 0; entry < 3; ++entry) {
        image.half(MAP_BLOCK + 0x10 + entry * 4, codes[entry]);
        image.half(MAP_BLOCK + 0x10 + entry * 4 + 2, indices[entry]);
    }
    GuestMemory memory{read_image, &image};

    GuestResFontGlyph glyph{};
    assert(read_guest_res_font_glyph(memory, FONT, 'A', glyph) == GuestResFontError::None);
    assert(glyph.mapping == GuestFontMapping::SortedPairs);
    assert(glyph.fontCode == 19);

    // A code inside the block's range that the pairs do not list leaves the default standing, which
    // is the guest's behaviour and not a refusal.
    assert(read_guest_res_font_glyph(memory, FONT, 'B', glyph) == GuestResFontError::None);
    assert(glyph.mapping == GuestFontMapping::DefaultCode);
    assert(glyph.fontCode == DEFAULT_CODE);
}

// A two-byte font whose lowest mapped code is itself two-byte folds ASCII onto full width before it
// consults any map. 'A' becomes 0x8260, which only the folded table can produce.
void folds_ascii_onto_full_width_for_a_two_byte_font() {
    Image image = western_font();
    image.half(INFO + 0x08, 2);
    image.half(FONT + 0x64, 0x8140);
    image.half(MAP_BLOCK + 0x0A, 0x8140);
    image.half(MAP_BLOCK + 0x0C, 0x82FF);
    GuestMemory memory{read_image, &image};

    GuestResFontGlyph glyph{};
    assert(read_guest_res_font_glyph(memory, FONT, 'A', glyph) == GuestResFontError::None);
    assert(glyph.mapping == GuestFontMapping::Direct);
    assert(glyph.fontCode == 0x8260 - 0x8140);
}

void refuses_a_font_whose_arithmetic_has_no_answer() {
    Image image = western_font();
    GuestMemory memory{read_image, &image};
    GuestResFontGlyph glyph{};

    assert(read_guest_res_font_glyph({nullptr, nullptr}, FONT, 'A', glyph) ==
           GuestResFontError::NoReader);
    assert(read_guest_res_font_glyph(memory, 0, 'A', glyph) == GuestResFontError::NullFont);
    assert(read_guest_res_font_glyph(memory, 0x70000000, 'A', glyph) ==
           GuestResFontError::UnreadableFont);
    assert(glyph.font == 0);

    image.word(FONT + 0x4C, 0);
    assert(read_guest_res_font_glyph(memory, FONT, 'A', glyph) == GuestResFontError::NullInfoBlock);

    image.word(FONT + 0x4C, 0x70000000);
    assert(read_guest_res_font_glyph(memory, FONT, 'A', glyph) ==
           GuestResFontError::UnreadableInfoBlock);

    image.word(FONT + 0x4C, INFO);
    image.half(INFO + 0x0E, 0);
    assert(read_guest_res_font_glyph(memory, FONT, 'A', glyph) ==
           GuestResFontError::DegenerateFontMetrics);

    image.half(INFO + 0x0E, FONT_WIDTH);
    image.half(INFO + 0x0A, 0);
    image.half(INFO + 0x0C, 0);
    assert(read_guest_res_font_glyph(memory, FONT, 'A', glyph) ==
           GuestResFontError::DegenerateFontMetrics);

    image.half(INFO + 0x0A, 12);
    image.half(INFO + 0x0C, 4);
    image.half(GLYPH_BLOCK + 0x16, 0);
    assert(read_guest_res_font_glyph(memory, FONT, 'A', glyph) ==
           GuestResFontError::DegenerateGlyphPage);

    image.half(GLYPH_BLOCK + 0x16, ROWS);
    image.word(GLYPH_TABLE, 0);
    assert(read_guest_res_font_glyph(memory, FONT, 'A', glyph) ==
           GuestResFontError::UnreadableBlock);
}

} // namespace

int main() {
    places_a_glyph_in_the_cell_the_guest_would_load();
    substitutes_the_font_width_where_no_entry_exists();
    keeps_the_loaded_page_when_no_block_covers_the_code();
    falls_back_to_the_fonts_own_default_code();
    searches_the_sorted_pairs_a_table_font_carries();
    folds_ascii_onto_full_width_for_a_two_byte_font();
    refuses_a_font_whose_arithmetic_has_no_answer();
    return 0;
}
