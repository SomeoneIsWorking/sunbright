#include <sunbright/title_adapter/guest_jut_texture.h>

namespace sb::title_adapter {
namespace {

[[nodiscard]] GuestJutTextureError read_palette(const GuestReader& reader, GuestAddress palette,
                                                GuestJutPalette& out) noexcept {
    out = {};
    if (palette == 0) {
        return GuestJutTextureError::None;
    }
    GuestJutPalette value{};
    value.address = palette;
    if (!reader.word(palette + GUEST_JUT_PALETTE_FORMAT, value.format) ||
        !reader.word(palette + GUEST_JUT_PALETTE_COLOR_TABLE, value.colorTable) ||
        !reader.half(palette + GUEST_JUT_PALETTE_ENTRIES, value.entries)) {
        return GuestJutTextureError::UnreadablePalette;
    }
    // A palette object that names no colours cannot be sampled through, and reporting it as one
    // that simply is not there would hand the decoder an indexed image with nothing to index.
    if (value.colorTable == 0) {
        return GuestJutTextureError::NullPaletteColorTable;
    }
    out = value;
    return GuestJutTextureError::None;
}

} // namespace

const char* name(GuestJutTextureError error) noexcept {
    switch (error) {
    case GuestJutTextureError::None:
        return "none";
    case GuestJutTextureError::NullTexture:
        return "null_texture";
    case GuestJutTextureError::UnreadableTexture:
        return "unreadable_texture";
    case GuestJutTextureError::UnreadablePalette:
        return "unreadable_palette";
    case GuestJutTextureError::NullPaletteColorTable:
        return "null_palette_color_table";
    }
    return "unknown";
}

GuestJutTextureError read_guest_jut_texture(const GuestReader& reader, GuestAddress texture,
                                            GuestJutTexture& out) noexcept {
    out = {};
    if (texture == 0) {
        return GuestJutTextureError::NullTexture;
    }
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
        return GuestJutTextureError::UnreadableTexture;
    }
    const GuestJutTextureError error = read_palette(reader, palette, value.palette);
    if (error != GuestJutTextureError::None) {
        return error;
    }
    out = value;
    return GuestJutTextureError::None;
}

} // namespace sb::title_adapter
