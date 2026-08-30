#include "resource_font_glyph_adapter.h"

#include <sunbright/native_render/image_decode.h>

#include <cstddef>
#include <limits>
#include <utility>

namespace sb::recomp {
namespace {

struct GlyphBlock {
    std::uint32_t address = 0;
    std::uint16_t startCode = 0;
    std::uint16_t endCode = 0;
    std::uint16_t cellWidth = 0;
    std::uint16_t cellHeight = 0;
    std::uint32_t textureSize = 0;
    std::uint16_t textureFormat = 0;
    std::uint16_t rows = 0;
    std::uint16_t columns = 0;
    std::uint16_t textureWidth = 0;
    std::uint16_t textureHeight = 0;
};

bool read_glyph_block(const BigEndianGuestReader& reader, std::uint32_t address,
                      GlyphBlock& block) noexcept {
    GlyphBlock result{.address = address};
    if (address == 0 || !reader.u16(address + 0x08, result.startCode) ||
        !reader.u16(address + 0x0a, result.endCode) ||
        !reader.u16(address + 0x0c, result.cellWidth) ||
        !reader.u16(address + 0x0e, result.cellHeight) ||
        !reader.u32(address + 0x10, result.textureSize) ||
        !reader.u16(address + 0x14, result.textureFormat) ||
        !reader.u16(address + 0x16, result.rows) || !reader.u16(address + 0x18, result.columns) ||
        !reader.u16(address + 0x1a, result.textureWidth) ||
        !reader.u16(address + 0x1c, result.textureHeight) || result.startCode > result.endCode ||
        result.cellWidth == 0 || result.cellHeight == 0 || result.textureSize == 0 ||
        result.rows == 0 || result.columns == 0 || result.textureWidth == 0 ||
        result.textureHeight == 0) {
        return false;
    }
    block = result;
    return true;
}

bool selected_font_code(std::int32_t cellX, std::int32_t cellY, std::int32_t page,
                        const GlyphBlock& block, std::uint32_t& code) noexcept {
    if (cellX < 0 || cellY < 0 || page < 0 || cellX % block.cellWidth != 0 ||
        cellY % block.cellHeight != 0) {
        return false;
    }
    const std::uint64_t column = static_cast<std::uint32_t>(cellX) / block.cellWidth;
    const std::uint64_t row = static_cast<std::uint32_t>(cellY) / block.cellHeight;
    // This deliberately follows the retail loadImage body, which uses numRows as the per-row
    // divisor/multiplier and rows*columns as the page size.
    if (column >= block.rows || row >= block.columns)
        return false;
    const std::uint64_t pageCells = static_cast<std::uint64_t>(block.rows) * block.columns;
    const std::uint64_t selected = static_cast<std::uint64_t>(block.startCode) +
                                   static_cast<std::uint64_t>(page) * pageCells + row * block.rows +
                                   column;
    if (selected > block.endCode || selected > std::numeric_limits<std::uint32_t>::max())
        return false;
    code = static_cast<std::uint32_t>(selected);
    return true;
}

bool read_width(const BigEndianGuestReader& reader, std::uint32_t self, std::uint32_t fontCode,
                std::uint32_t defaultWidth, std::uint32_t& bearing,
                std::uint32_t& glyphWidth) noexcept {
    std::uint32_t blocks = 0;
    std::uint16_t count = 0;
    if (!reader.u32(self + 0x50, blocks) || !reader.u16(self + 0x5c, count) ||
        (count != 0 && blocks == 0)) {
        return false;
    }
    bearing = 0;
    glyphWidth = defaultWidth;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t block = 0;
        std::uint16_t start = 0;
        std::uint16_t end = 0;
        if (!reader.u32(blocks + index * 4U, block) || block == 0 ||
            !reader.u16(block + 0x08, start) || !reader.u16(block + 0x0a, end) || start > end) {
            return false;
        }
        if (fontCode < start || fontCode > end)
            continue;
        const std::uint32_t entry = block + 0x0c + (fontCode - start) * 2U;
        std::uint8_t left = 0;
        std::uint8_t width = 0;
        if (!reader.u8(entry, left) || !reader.u8(entry + 1U, width))
            return false;
        bearing = left;
        glyphWidth = width;
        return true;
    }
    return true;
}

bool format_has_alpha(native_render::EncodedImageFormat format) noexcept {
    return format != native_render::EncodedImageFormat::Rgb565;
}

} // namespace

void CapturedGlyph::refresh_image_view() noexcept {
    image = {command.atlas.resource, command.atlas.revision, command.atlas.width,
             command.atlas.height, rgba8};
}

bool capture_resource_font_glyph(const GuestByteReader& byteReader, std::uint32_t self,
                                 const ResourceGlyphArgs& args,
                                 const native_render::Matrix3x4& transform,
                                 native_render::Color black, native_render::Color white,
                                 CapturedGlyph& capture) noexcept {
    if (self == 0 || !native_render::valid(transform))
        return false;
    const BigEndianGuestReader reader(byteReader);

    std::uint8_t fixed = 0;
    std::int32_t fixedWidth = 0;
    std::int32_t cellX = 0;
    std::int32_t cellY = 0;
    std::int32_t page = 0;
    std::uint32_t info = 0;
    std::uint32_t glyphBlocks = 0;
    std::uint16_t glyphBlockIndex = 0;
    std::uint16_t fontWidth = 0;
    std::uint16_t ascent = 0;
    std::uint16_t descent = 0;
    if (!reader.u8(self + 0x04, fixed) || !reader.s32(self + 0x08, fixedWidth) ||
        !reader.s32(self + 0x1c, cellX) || !reader.s32(self + 0x20, cellY) ||
        !reader.s32(self + 0x44, page) || !reader.u32(self + 0x4c, info) || info == 0 ||
        !reader.u32(self + 0x54, glyphBlocks) || glyphBlocks == 0 ||
        !reader.u16(self + 0x62, glyphBlockIndex) || !reader.u16(info + 0x0e, fontWidth) ||
        !reader.u16(info + 0x0a, ascent) || !reader.u16(info + 0x0c, descent) || fontWidth == 0 ||
        static_cast<std::uint32_t>(ascent) + descent == 0 || fixedWidth < 0) {
        return false;
    }

    std::uint32_t glyphBlockAddress = 0;
    GlyphBlock glyphBlock{};
    if (!reader.u32(glyphBlocks + static_cast<std::uint32_t>(glyphBlockIndex) * 4U,
                    glyphBlockAddress) ||
        !read_glyph_block(reader, glyphBlockAddress, glyphBlock)) {
        return false;
    }
    std::uint32_t fontCode = 0;
    std::uint32_t bearing = 0;
    std::uint32_t glyphWidth = 0;
    if (!selected_font_code(cellX, cellY, page, glyphBlock, fontCode) ||
        !read_width(reader, self, fontCode, fontWidth, bearing, glyphWidth)) {
        return false;
    }

    native_render::EncodedImageFormat format{};
    std::size_t sourceBytes = 0;
    std::size_t outputBytes = 0;
    if (glyphBlock.textureFormat > std::numeric_limits<std::uint8_t>::max() ||
        !native_render::decode_image_format(static_cast<std::uint8_t>(glyphBlock.textureFormat),
                                            format) ||
        format == native_render::EncodedImageFormat::Indexed4 ||
        format == native_render::EncodedImageFormat::Indexed8 ||
        format == native_render::EncodedImageFormat::Indexed14 ||
        !native_render::encoded_image_data_size(glyphBlock.textureWidth, glyphBlock.textureHeight,
                                                format, sourceBytes) ||
        sourceBytes > glyphBlock.textureSize ||
        !native_render::decoded_image_data_size(glyphBlock.textureWidth, glyphBlock.textureHeight,
                                                outputBytes)) {
        return false;
    }
    const std::uint64_t pageAddress64 = static_cast<std::uint64_t>(glyphBlock.address) + 0x20U +
                                        static_cast<std::uint64_t>(page) * glyphBlock.textureSize;
    if (pageAddress64 > std::numeric_limits<std::uint32_t>::max())
        return false;
    const auto pageAddress = static_cast<std::uint32_t>(pageAddress64);
    std::vector<std::uint8_t> encoded(sourceBytes);
    if (!reader.bytes(pageAddress, encoded.data(), encoded.size()))
        return false;
    const native_render::EncodedImageView source{format, glyphBlock.textureWidth,
                                                 glyphBlock.textureHeight, encoded};

    CapturedGlyph result{};
    result.rgba8.resize(outputBytes);
    if (native_render::decode_image_rgba8(source, result.rgba8) !=
        native_render::ImageDecodeError::None) {
        return false;
    }

    native_render::ResourceGlyphLayout layout{.positionX = args.positionX,
                                              .positionY = args.positionY,
                                              .scaleX = args.scaleX,
                                              .scaleY = args.scaleY,
                                              .fontWidth = fontWidth,
                                              .fontHeight =
                                                  static_cast<std::uint32_t>(ascent) + descent,
                                              .ascent = ascent,
                                              .descent = descent,
                                              .leftBearing = bearing,
                                              .glyphWidth = glyphWidth,
                                              .fixedWidth = static_cast<std::uint32_t>(fixedWidth),
                                              .fixed = fixed != 0,
                                              .applyBearing = args.applyBearing,
                                              .cellX = static_cast<std::uint32_t>(cellX),
                                              .cellY = static_cast<std::uint32_t>(cellY),
                                              .atlasWidth = glyphBlock.textureWidth,
                                              .atlasHeight = glyphBlock.textureHeight,
                                              .transform = transform};
    native_render::ResolvedGlyphLayout resolved{};
    if (!native_render::resolve_resource_glyph_layout(layout, resolved))
        return false;

    result.command.instance = self;
    result.command.code = args.code;
    result.command.positions = resolved.positions;
    result.command.uv = resolved.uv;
    result.command.atlas = {.resource = pageAddress,
                            .width = glyphBlock.textureWidth,
                            .height = glyphBlock.textureHeight,
                            .minFilter = native_render::FilterMode::Linear,
                            .magFilter = native_render::FilterMode::Linear,
                            .hasAlpha = format_has_alpha(format)};
    if (!native_render::image_content_revision(source, result.command.atlas.revision))
        return false;
    for (std::size_t index = 0; index < result.command.corner.size(); ++index) {
        std::uint32_t rgba = 0;
        if (!reader.u32(self + 0x0c + static_cast<std::uint32_t>(index * 4U), rgba))
            return false;
        result.command.corner[index] = native_render::color_from_rgba8(rgba);
    }
    result.command.black = black;
    result.command.white = white;
    if (!native_render::valid(result.command))
        return false;
    capture = std::move(result);
    capture.refresh_image_view();
    return true;
}

} // namespace sb::recomp
