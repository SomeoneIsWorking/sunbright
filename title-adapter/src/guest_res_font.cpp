#include <sunbright/title_adapter/guest_res_font.h>

namespace sb::title_adapter {
namespace {

constexpr std::size_t FONT_CORNERS = 4;
constexpr GuestAddress POINTER_BYTES = 4;
constexpr GuestAddress HALF_BYTES = 2;

// `JUTResFont::getFontCode` folds the ASCII range onto full-width Shift-JIS before it consults any
// map, for a two-byte font whose lowest mapped code is itself two-byte. The table is the guest's,
// transcribed rather than regenerated: it is not a formula, and the six entries that are not in
// code-point order (the punctuation at 0x20-0x2F) are exactly where a regenerated one would differ.
constexpr std::uint16_t HALF_TO_FULL_WIDTH[95] = {
    0x8140, 0x8149, 0x8168, 0x8194, 0x8190, 0x8193, 0x8195, 0x8166, 0x8169, 0x816A, 0x8196, 0x817B,
    0x8143, 0x817C, 0x8144, 0x815E, 0x824F, 0x8250, 0x8251, 0x8252, 0x8253, 0x8254, 0x8255, 0x8256,
    0x8257, 0x8258, 0x8146, 0x8147, 0x8183, 0x8181, 0x8184, 0x8148, 0x8197, 0x8260, 0x8261, 0x8262,
    0x8263, 0x8264, 0x8265, 0x8266, 0x8267, 0x8268, 0x8269, 0x826A, 0x826B, 0x826C, 0x826D, 0x826E,
    0x826F, 0x8270, 0x8271, 0x8272, 0x8273, 0x8274, 0x8275, 0x8276, 0x8277, 0x8278, 0x8279, 0x816D,
    0x818F, 0x816E, 0x814F, 0x8151, 0x8165, 0x8281, 0x8282, 0x8283, 0x8284, 0x8285, 0x8286, 0x8287,
    0x8288, 0x8289, 0x828A, 0x828B, 0x828C, 0x828D, 0x828E, 0x828F, 0x8290, 0x8291, 0x8292, 0x8293,
    0x8294, 0x8295, 0x8296, 0x8297, 0x8298, 0x8299, 0x829A, 0x816F, 0x8162, 0x8170, 0x8160,
};

constexpr std::uint32_t FIRST_FOLDED_CODE = 0x20;
constexpr std::uint32_t LAST_FOLDED_CODE = 0x7F;
constexpr std::uint16_t TWO_BYTE_FONT_TYPE = 2;
constexpr std::uint16_t LOWEST_TWO_BYTE_CODE = 0x8000;

// `JUTResFont::convertSjis`.
[[nodiscard]] std::uint32_t convert_shift_jis(std::uint32_t code) noexcept {
    const std::int32_t high = static_cast<std::int32_t>((code >> 8U) & 0xFFU);
    std::int32_t low = static_cast<std::int32_t>(code & 0xFFU) - 0x40;
    if (low >= 0x40) {
        low -= 1;
    }
    return static_cast<std::uint32_t>(low + (high - 0x88) * 0xBC + 0x2BE);
}

// One entry of a `WID1`/`GLY1`/`MAP1` pointer table.
[[nodiscard]] bool read_block_pointer(const GuestReader& reader, GuestAddress table,
                                      std::uint16_t index, GuestAddress& block) noexcept {
    return reader.word(table + static_cast<GuestAddress>(index) * POINTER_BYTES, block);
}

[[nodiscard]] bool block_range(const GuestReader& reader, GuestAddress block, GuestAddress startAt,
                               GuestAddress endAt, std::uint16_t& start,
                               std::uint16_t& end) noexcept {
    return reader.half(block + startAt, start) && reader.half(block + endAt, end);
}

} // namespace

const char* name(GuestResFontError error) noexcept {
    switch (error) {
    case GuestResFontError::None:
        return "none";
    case GuestResFontError::NoReader:
        return "no_reader";
    case GuestResFontError::NullFont:
        return "null_font";
    case GuestResFontError::UnreadableFont:
        return "unreadable_font";
    case GuestResFontError::NullInfoBlock:
        return "null_info_block";
    case GuestResFontError::UnreadableInfoBlock:
        return "unreadable_info_block";
    case GuestResFontError::UnreadableBlockTable:
        return "unreadable_block_table";
    case GuestResFontError::UnreadableBlock:
        return "unreadable_block";
    case GuestResFontError::DegenerateFontMetrics:
        return "degenerate_font_metrics";
    case GuestResFontError::DegenerateGlyphPage:
        return "degenerate_glyph_page";
    }
    return "unknown";
}

const char* name(GuestFontMapping mapping) noexcept {
    switch (mapping) {
    case GuestFontMapping::DefaultCode:
        return "default_code";
    case GuestFontMapping::Direct:
        return "direct";
    case GuestFontMapping::ShiftJis:
        return "shift_jis";
    case GuestFontMapping::Table:
        return "table";
    case GuestFontMapping::SortedPairs:
        return "sorted_pairs";
    }
    return "unknown";
}

namespace {

// `JUTResFont::getFontCode`.
[[nodiscard]] GuestResFontError
resolve_font_code(const GuestReader& reader, GuestAddress mapTable, std::uint16_t mapCount,
                  std::uint16_t fontType, std::uint16_t lowestMappedCode, std::uint16_t defaultCode,
                  std::uint32_t code, std::uint32_t& fontCode, GuestFontMapping& mapping) noexcept {
    fontCode = defaultCode;
    mapping = GuestFontMapping::DefaultCode;

    std::uint32_t mapped = code;
    if (fontType == TWO_BYTE_FONT_TYPE && lowestMappedCode >= LOWEST_TWO_BYTE_CODE &&
        code >= FIRST_FOLDED_CODE && code < LAST_FOLDED_CODE) {
        mapped = HALF_TO_FULL_WIDTH[code - FIRST_FOLDED_CODE];
    }

    for (std::uint16_t index = 0; index < mapCount; ++index) {
        GuestAddress block = 0;
        if (!read_block_pointer(reader, mapTable, index, block)) {
            return GuestResFontError::UnreadableBlockTable;
        }
        if (block == 0) {
            return GuestResFontError::UnreadableBlock;
        }
        std::uint16_t start = 0;
        std::uint16_t end = 0;
        std::uint16_t method = 0;
        std::uint16_t entries = 0;
        if (!block_range(reader, block, GUEST_FONT_MAP_START_CODE, GUEST_FONT_MAP_END_CODE, start,
                         end) ||
            !reader.half(block + GUEST_FONT_MAP_METHOD, method) ||
            !reader.half(block + GUEST_FONT_MAP_ENTRY_COUNT, entries)) {
            return GuestResFontError::UnreadableBlock;
        }
        if (mapped < start || mapped > end) {
            continue;
        }

        switch (method) {
        case 0:
            fontCode = mapped - start;
            mapping = GuestFontMapping::Direct;
            return GuestResFontError::None;
        case 1:
            fontCode = convert_shift_jis(mapped);
            mapping = GuestFontMapping::ShiftJis;
            return GuestResFontError::None;
        case 2: {
            std::uint16_t entry = 0;
            if (!reader.half(block + GUEST_FONT_MAP_ENTRIES +
                                 static_cast<GuestAddress>(mapped - start) * HALF_BYTES,
                             entry)) {
                return GuestResFontError::UnreadableBlock;
            }
            fontCode = entry;
            mapping = GuestFontMapping::Table;
            return GuestResFontError::None;
        }
        case 3: {
            // The guest's own binary search over (code, index) pairs. A code the search does not
            // find leaves the default standing, which is why the mapping is only claimed on a hit.
            std::int32_t low = 0;
            std::int32_t high = static_cast<std::int32_t>(entries) - 1;
            while (high >= low) {
                const std::int32_t middle = (high + low) / 2;
                std::uint16_t key = 0;
                if (!reader.half(block + GUEST_FONT_MAP_ENTRIES +
                                     static_cast<GuestAddress>(middle) * 2 * HALF_BYTES,
                                 key)) {
                    return GuestResFontError::UnreadableBlock;
                }
                if (mapped < key) {
                    high = middle - 1;
                    continue;
                }
                if (mapped > key) {
                    low = middle + 1;
                    continue;
                }
                std::uint16_t value = 0;
                if (!reader.half(block + GUEST_FONT_MAP_ENTRIES +
                                     (static_cast<GuestAddress>(middle) * 2 + 1) * HALF_BYTES,
                                 value)) {
                    return GuestResFontError::UnreadableBlock;
                }
                fontCode = value;
                mapping = GuestFontMapping::SortedPairs;
                break;
            }
            return GuestResFontError::None;
        }
        default:
            // An unknown method leaves the default code, exactly as the guest's `if` chain does.
            return GuestResFontError::None;
        }
    }
    return GuestResFontError::None;
}

// `JUTResFont::loadFont`'s width-entry walk.
[[nodiscard]] GuestResFontError
resolve_width_entry(const GuestReader& reader, GuestAddress widthTable, std::uint16_t widthCount,
                    std::uint32_t fontCode, GuestResFontGlyph& glyph) noexcept {
    for (std::uint16_t index = 0; index < widthCount; ++index) {
        GuestAddress block = 0;
        if (!read_block_pointer(reader, widthTable, index, block)) {
            return GuestResFontError::UnreadableBlockTable;
        }
        if (block == 0) {
            return GuestResFontError::UnreadableBlock;
        }
        std::uint16_t start = 0;
        std::uint16_t end = 0;
        if (!block_range(reader, block, GUEST_FONT_WIDTH_START_CODE, GUEST_FONT_WIDTH_END_CODE,
                         start, end)) {
            return GuestResFontError::UnreadableBlock;
        }
        if (fontCode < start || fontCode > end) {
            continue;
        }
        const GuestAddress entry =
            block + GUEST_FONT_WIDTH_ENTRIES + static_cast<GuestAddress>(fontCode - start) * 2;
        if (!reader.byte(entry, glyph.leftBearing) || !reader.byte(entry + 1, glyph.glyphWidth)) {
            return GuestResFontError::UnreadableBlock;
        }
        glyph.hasWidthEntry = true;
        return GuestResFontError::None;
    }
    // No block covers the code: `loadFont` substitutes a zero bearing and the font's own width.
    glyph.leftBearing = 0;
    glyph.glyphWidth = static_cast<std::uint8_t>(glyph.fontWidth);
    glyph.hasWidthEntry = false;
    return GuestResFontError::None;
}

// `JUTResFont::loadImage`.
[[nodiscard]] GuestResFontError resolve_glyph_page(const GuestReader& reader,
                                                   GuestAddress glyphTable,
                                                   std::uint16_t glyphCount, std::uint32_t fontCode,
                                                   GuestResFontGlyph& glyph) noexcept {
    for (std::uint16_t index = 0; index < glyphCount; ++index) {
        GuestAddress block = 0;
        if (!read_block_pointer(reader, glyphTable, index, block)) {
            return GuestResFontError::UnreadableBlockTable;
        }
        if (block == 0) {
            return GuestResFontError::UnreadableBlock;
        }
        std::uint16_t start = 0;
        std::uint16_t end = 0;
        if (!block_range(reader, block, GUEST_FONT_GLYPH_START_CODE, GUEST_FONT_GLYPH_END_CODE,
                         start, end)) {
            return GuestResFontError::UnreadableBlock;
        }
        if (fontCode < start || fontCode > end) {
            continue;
        }

        GuestGlyphPage page{};
        page.block = block;
        page.blockIndex = index;
        if (!reader.half(block + GUEST_FONT_GLYPH_CELL_WIDTH, page.cellWidth) ||
            !reader.half(block + GUEST_FONT_GLYPH_CELL_HEIGHT, page.cellHeight) ||
            !reader.word(block + GUEST_FONT_GLYPH_TEXTURE_SIZE, page.textureSize) ||
            !reader.half(block + GUEST_FONT_GLYPH_TEXTURE_WIDTH, page.textureWidth) ||
            !reader.half(block + GUEST_FONT_GLYPH_TEXTURE_HEIGHT, page.textureHeight)) {
            return GuestResFontError::UnreadableBlock;
        }
        std::uint16_t format = 0;
        std::uint16_t rows = 0;
        std::uint16_t columns = 0;
        if (!reader.half(block + GUEST_FONT_GLYPH_TEXTURE_FORMAT, format) ||
            !reader.half(block + GUEST_FONT_GLYPH_ROWS, rows) ||
            !reader.half(block + GUEST_FONT_GLYPH_COLUMNS, columns)) {
            return GuestResFontError::UnreadableBlock;
        }
        page.textureFormat = format;
        if (rows == 0 || columns == 0 || page.textureWidth == 0 || page.textureHeight == 0 ||
            page.textureSize == 0) {
            return GuestResFontError::DegenerateGlyphPage;
        }

        // The guest's cell arithmetic, transcribed including the asymmetry: the row is the cell
        // index divided by `numRows` rather than by `numColumns`, and the column is the remainder.
        const std::uint32_t cellsPerPage = static_cast<std::uint32_t>(rows) * columns;
        const std::uint32_t inBlock = fontCode - start;
        const std::uint32_t pageIndex = inBlock / cellsPerPage;
        const std::uint32_t inPage = inBlock % cellsPerPage;
        const std::uint32_t cellRow = inPage / rows;
        const std::uint32_t cellColumn = inPage - cellRow * rows;
        page.pageIndex = static_cast<std::uint16_t>(pageIndex);
        page.data = block + GUEST_FONT_GLYPH_DATA + pageIndex * page.textureSize;
        glyph.cellX = cellColumn * page.cellWidth;
        glyph.cellY = cellRow * page.cellHeight;
        glyph.page = page;
        return GuestResFontError::None;
    }
    return GuestResFontError::None;
}

} // namespace

GuestResFontError read_guest_res_font_glyph(const GuestMemory& memory, GuestAddress font,
                                            std::uint32_t code, GuestResFontGlyph& out) noexcept {
    if (memory.read == nullptr) {
        return GuestResFontError::NoReader;
    }
    if (font == 0) {
        return GuestResFontError::NullFont;
    }
    const GuestReader reader(memory);

    GuestResFontGlyph glyph{};
    glyph.font = font;
    glyph.code = code;

    GuestAddress infoBlock = 0;
    GuestAddress widthTable = 0;
    GuestAddress glyphTable = 0;
    GuestAddress mapTable = 0;
    std::uint16_t widthCount = 0;
    std::uint16_t glyphCount = 0;
    std::uint16_t mapCount = 0;
    std::uint16_t lowestMappedCode = 0;
    std::uint8_t fixed = 0;
    if (!reader.byte(font + GUEST_FONT_FIXED, fixed) ||
        !reader.word(font + GUEST_FONT_FIXED_WIDTH, glyph.fixedWidth) ||
        !reader.word(font + GUEST_RES_FONT_INFO_BLOCK, infoBlock) ||
        !reader.word(font + GUEST_RES_FONT_WIDTH_BLOCKS, widthTable) ||
        !reader.word(font + GUEST_RES_FONT_GLYPH_BLOCKS, glyphTable) ||
        !reader.word(font + GUEST_RES_FONT_MAP_BLOCKS, mapTable) ||
        !reader.half(font + GUEST_RES_FONT_WIDTH_BLOCK_COUNT, widthCount) ||
        !reader.half(font + GUEST_RES_FONT_GLYPH_BLOCK_COUNT, glyphCount) ||
        !reader.half(font + GUEST_RES_FONT_MAP_BLOCK_COUNT, mapCount) ||
        !reader.half(font + GUEST_RES_FONT_LOWEST_MAPPED_CODE, lowestMappedCode)) {
        return GuestResFontError::UnreadableFont;
    }
    glyph.fixed = fixed != 0;
    for (std::size_t corner = 0; corner < FONT_CORNERS; ++corner) {
        if (!reader.word(font + GUEST_FONT_COLORS + static_cast<GuestAddress>(corner) * 4,
                         glyph.corner[corner])) {
            return GuestResFontError::UnreadableFont;
        }
    }

    if (infoBlock == 0) {
        return GuestResFontError::NullInfoBlock;
    }
    std::uint16_t defaultCode = 0;
    if (!reader.half(infoBlock + GUEST_FONT_INFO_TYPE, glyph.fontType) ||
        !reader.half(infoBlock + GUEST_FONT_INFO_ASCENT, glyph.ascent) ||
        !reader.half(infoBlock + GUEST_FONT_INFO_DESCENT, glyph.descent) ||
        !reader.half(infoBlock + GUEST_FONT_INFO_WIDTH, glyph.fontWidth) ||
        !reader.half(infoBlock + GUEST_FONT_INFO_LEADING, glyph.leading) ||
        !reader.half(infoBlock + GUEST_FONT_INFO_DEFAULT_CODE, defaultCode)) {
        return GuestResFontError::UnreadableInfoBlock;
    }
    // `drawChar_scale` divides the requested scale by both of these.
    if (glyph.fontWidth == 0 || glyph.ascent + glyph.descent == 0) {
        return GuestResFontError::DegenerateFontMetrics;
    }

    GuestResFontError error =
        resolve_font_code(reader, mapTable, mapCount, glyph.fontType, lowestMappedCode, defaultCode,
                          code, glyph.fontCode, glyph.mapping);
    if (error != GuestResFontError::None) {
        return error;
    }
    error = resolve_width_entry(reader, widthTable, widthCount, glyph.fontCode, glyph);
    if (error != GuestResFontError::None) {
        return error;
    }
    error = resolve_glyph_page(reader, glyphTable, glyphCount, glyph.fontCode, glyph);
    if (error != GuestResFontError::None) {
        return error;
    }

    if (glyph.page.block == 0) {
        // No `GLY1` block covers the code. `loadImage` returns without touching what is loaded, so
        // the guest draws the page and cell already selected -- read from the font rather than
        // chosen here, because a page this picked would be one the console never bound.
        std::uint16_t blockIndex = 0;
        std::uint32_t pageIndex = 0;
        if (!reader.half(font + GUEST_RES_FONT_GLYPH_BLOCK_INDEX, blockIndex) ||
            !reader.word(font + GUEST_RES_FONT_PAGE_INDEX, pageIndex) ||
            !reader.word(font + GUEST_RES_FONT_CELL_X, glyph.cellX) ||
            !reader.word(font + GUEST_RES_FONT_CELL_Y, glyph.cellY)) {
            return GuestResFontError::UnreadableFont;
        }
        if (blockIndex >= glyphCount) {
            return GuestResFontError::DegenerateGlyphPage;
        }
        GuestAddress block = 0;
        if (!read_block_pointer(reader, glyphTable, blockIndex, block)) {
            return GuestResFontError::UnreadableBlockTable;
        }
        if (block == 0) {
            return GuestResFontError::UnreadableBlock;
        }
        GuestGlyphPage page{};
        page.block = block;
        page.blockIndex = blockIndex;
        page.pageIndex = static_cast<std::uint16_t>(pageIndex);
        page.retained = true;
        std::uint16_t format = 0;
        if (!reader.half(block + GUEST_FONT_GLYPH_CELL_WIDTH, page.cellWidth) ||
            !reader.half(block + GUEST_FONT_GLYPH_CELL_HEIGHT, page.cellHeight) ||
            !reader.word(block + GUEST_FONT_GLYPH_TEXTURE_SIZE, page.textureSize) ||
            !reader.half(block + GUEST_FONT_GLYPH_TEXTURE_FORMAT, format) ||
            !reader.half(block + GUEST_FONT_GLYPH_TEXTURE_WIDTH, page.textureWidth) ||
            !reader.half(block + GUEST_FONT_GLYPH_TEXTURE_HEIGHT, page.textureHeight)) {
            return GuestResFontError::UnreadableBlock;
        }
        page.textureFormat = format;
        if (page.textureWidth == 0 || page.textureHeight == 0 || page.textureSize == 0) {
            return GuestResFontError::DegenerateGlyphPage;
        }
        page.data = block + GUEST_FONT_GLYPH_DATA + pageIndex * page.textureSize;
        glyph.page = page;
    }

    out = glyph;
    return GuestResFontError::None;
}

} // namespace sb::title_adapter
