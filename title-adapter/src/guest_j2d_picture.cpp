#include <sunbright/title_adapter/guest_j2d_picture.h>

namespace sb::title_adapter {
namespace {

[[nodiscard]] bool read_matrix(const GuestReader& reader, GuestAddress address,
                               std::array<float, 12>& out) noexcept {
    for (std::size_t index = 0; index < out.size(); ++index) {
        if (!reader.real(address + static_cast<GuestAddress>(index * 4), out[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] GuestPictureError read_palette(const GuestReader& reader, GuestAddress palette,
                                             GuestJutPalette& out) noexcept {
    out = {};
    if (palette == 0) {
        return GuestPictureError::None;
    }
    GuestJutPalette value{};
    value.address = palette;
    if (!reader.word(palette + GUEST_JUT_PALETTE_FORMAT, value.format) ||
        !reader.word(palette + GUEST_JUT_PALETTE_COLOR_TABLE, value.colorTable) ||
        !reader.half(palette + GUEST_JUT_PALETTE_ENTRIES, value.entries)) {
        return GuestPictureError::UnreadablePalette;
    }
    // A palette object that names no colours cannot be sampled through, and reporting it as one
    // that simply is not there would hand the decoder an indexed image with nothing to index.
    if (value.colorTable == 0) {
        return GuestPictureError::NullPaletteColorTable;
    }
    out = value;
    return GuestPictureError::None;
}

[[nodiscard]] GuestPictureError read_texture(const GuestReader& reader, GuestAddress texture,
                                             GuestJutTexture& out) noexcept {
    GuestJutTexture value{};
    value.address = texture;
    GuestAddress palette = 0;
    if (!reader.word(texture + GUEST_JUT_TEXTURE_RESOURCE, value.resource) ||
        !reader.word(texture + GUEST_JUT_TEXTURE_DATA, value.data) ||
        !reader.word(texture + GUEST_JUT_TEXTURE_ACTIVE_PALETTE, palette) ||
        !reader.word(texture + GUEST_JUT_TEXTURE_FORMAT, value.format) ||
        !reader.word(texture + GUEST_JUT_TEXTURE_ALPHA_ENABLED, value.alphaEnabled) ||
        !reader.half(texture + GUEST_JUT_TEXTURE_WIDTH, value.width) ||
        !reader.half(texture + GUEST_JUT_TEXTURE_HEIGHT, value.height) ||
        !reader.byte(texture + GUEST_JUT_TEXTURE_WRAP_S, value.wrapS) ||
        !reader.byte(texture + GUEST_JUT_TEXTURE_WRAP_T, value.wrapT) ||
        !reader.byte(texture + GUEST_JUT_TEXTURE_MIN_FILTER, value.minFilter) ||
        !reader.byte(texture + GUEST_JUT_TEXTURE_MAG_FILTER, value.magFilter)) {
        return GuestPictureError::UnreadableTexture;
    }
    const GuestPictureError error = read_palette(reader, palette, value.palette);
    if (error != GuestPictureError::None) {
        return error;
    }
    out = value;
    return GuestPictureError::None;
}

} // namespace

const char* name(GuestPictureError error) noexcept {
    switch (error) {
    case GuestPictureError::None:
        return "none";
    case GuestPictureError::NoReader:
        return "no_reader";
    case GuestPictureError::NullPicture:
        return "null_picture";
    case GuestPictureError::UnreadablePicture:
        return "unreadable_picture";
    case GuestPictureError::TextureCountOutOfRange:
        return "texture_count_out_of_range";
    case GuestPictureError::NullTexture:
        return "null_texture";
    case GuestPictureError::UnreadableTexture:
        return "unreadable_texture";
    case GuestPictureError::UnreadablePalette:
        return "unreadable_palette";
    case GuestPictureError::NullPaletteColorTable:
        return "null_palette_color_table";
    case GuestPictureError::EmptyBounds:
        return "empty_bounds";
    }
    return "unknown";
}

bool read_guest_matrix(const GuestMemory& memory, GuestAddress matrix,
                       std::array<float, 12>& out) noexcept {
    if (memory.read == nullptr || matrix == 0) {
        return false;
    }
    return read_matrix(GuestReader(memory), matrix, out);
}

GuestPictureError read_guest_picture(const GuestMemory& memory, GuestAddress picture,
                                     GuestPicture& out) noexcept {
    if (memory.read == nullptr) {
        return GuestPictureError::NoReader;
    }
    if (picture == 0) {
        return GuestPictureError::NullPicture;
    }
    const GuestReader reader(memory);

    GuestPicture value{};
    value.address = picture;
    std::uint8_t flip = 0;
    if (!read_guest_rect(reader, picture + GUEST_PANE_BOUNDS, value.bounds) ||
        !read_guest_rect(reader, picture + GUEST_PANE_CLIP_RECT, value.clipRect) ||
        !read_matrix(reader, picture + GUEST_PANE_POSITION_MATRIX, value.positionMatrix) ||
        !read_matrix(reader, picture + GUEST_PANE_GLOBAL_MATRIX, value.globalMatrix) ||
        !reader.byte(picture + GUEST_PANE_COLOR_ALPHA, value.colorAlpha) ||
        !reader.byte(picture + GUEST_PICTURE_TEXTURE_COUNT, value.textureCount) ||
        !reader.word(picture + GUEST_PICTURE_BINDING, value.binding) ||
        !reader.word(picture + GUEST_PICTURE_MIRROR, value.mirror) ||
        !reader.byte(picture + GUEST_PICTURE_FLIP, flip) ||
        !reader.word(picture + GUEST_PICTURE_WHITE, value.white) ||
        !reader.word(picture + GUEST_PICTURE_BLACK, value.black) ||
        !reader.word(picture + GUEST_PICTURE_BLEND_KONST_COLOR, value.blendKonstColor) ||
        !reader.word(picture + GUEST_PICTURE_BLEND_KONST_ALPHA, value.blendKonstAlpha)) {
        return GuestPictureError::UnreadablePicture;
    }
    value.flip = flip != 0;

    std::uint32_t horizontal = 0;
    std::uint32_t vertical = 0;
    if (!reader.word(picture + GUEST_PICTURE_WRAP_HORIZONTAL, horizontal) ||
        !reader.word(picture + GUEST_PICTURE_WRAP_VERTICAL, vertical)) {
        return GuestPictureError::UnreadablePicture;
    }
    value.wrapHorizontal = static_cast<std::int32_t>(horizontal);
    value.wrapVertical = static_cast<std::int32_t>(vertical);

    for (std::size_t corner = 0; corner < value.cornerColors.size(); ++corner) {
        if (!reader.word(picture + GUEST_PICTURE_CORNER_COLORS +
                             static_cast<GuestAddress>(corner * 4),
                         value.cornerColors[corner])) {
            return GuestPictureError::UnreadablePicture;
        }
    }

    // An empty pane is not drawn by `J2DPane::draw` at all, so a reader that accepted one would be
    // producing a quad the title never had.
    if (value.bounds.empty()) {
        return GuestPictureError::EmptyBounds;
    }
    if (value.textureCount == 0 || value.textureCount > GUEST_MAX_PICTURE_TEXTURES) {
        return GuestPictureError::TextureCountOutOfRange;
    }

    for (std::size_t layer = 0; layer < value.textureCount; ++layer) {
        GuestAddress texture = 0;
        if (!reader.word(picture + GUEST_PICTURE_TEXTURES + static_cast<GuestAddress>(layer * 4),
                         texture)) {
            return GuestPictureError::UnreadablePicture;
        }
        if (texture == 0) {
            return GuestPictureError::NullTexture;
        }
        const GuestPictureError error = read_texture(reader, texture, value.textures[layer]);
        if (error != GuestPictureError::None) {
            return error;
        }
    }

    out = value;
    return GuestPictureError::None;
}

} // namespace sb::title_adapter
