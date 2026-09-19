#include <sunbright/title_adapter/guest_j3d_texture.h>

#include <sunbright/native_render/image_decode.h>

namespace sb::title_adapter {

const char* name(GuestTextureError error) noexcept {
    switch (error) {
    case GuestTextureError::None:
        return "none";
    case GuestTextureError::NoReader:
        return "no reader";
    case GuestTextureError::NullTable:
        return "null texture table";
    case GuestTextureError::UnreadableTable:
        return "unreadable texture table";
    case GuestTextureError::EmptyTable:
        return "empty texture table";
    case GuestTextureError::TextureNumberOutOfRange:
        return "texture number out of range";
    case GuestTextureError::NullResourceArray:
        return "null resource array";
    case GuestTextureError::DecodeFailed:
        return "decode failed";
    }
    return "unknown";
}

GuestTextureError read_guest_texture_table(const GuestMemory& memory, GuestAddress table,
                                           GuestTextureTable& out) noexcept {
    if (memory.read == nullptr) {
        return GuestTextureError::NoReader;
    }
    if (table == 0) {
        return GuestTextureError::NullTable;
    }
    const GuestReader reader(memory);
    GuestTextureTable result{};
    result.address = table;
    if (!reader.half(table + GUEST_TEXTURE_TABLE_COUNT, result.count) ||
        !reader.half(table + GUEST_TEXTURE_TABLE_COUNT + 2, result.padding) ||
        !reader.word(table + GUEST_TEXTURE_TABLE_RESOURCES, result.resources)) {
        return GuestTextureError::UnreadableTable;
    }
    if (result.resources == 0) {
        return GuestTextureError::NullResourceArray;
    }
    if (result.count == 0) {
        return GuestTextureError::EmptyTable;
    }
    out = result;
    return GuestTextureError::None;
}

GuestTextureError decode_guest_texture(const native_render::AssetByteSource& source,
                                       const GuestTextureTable& table, std::uint16_t textureNumber,
                                       native_render::DecodedTexture& decoded,
                                       native_render::ResTimgDecodeError& textureError) noexcept {
    if (source.read == nullptr) {
        return GuestTextureError::NoReader;
    }
    if (table.resources == 0) {
        return GuestTextureError::NullResourceArray;
    }
    if (textureNumber >= table.count) {
        return GuestTextureError::TextureNumberOutOfRange;
    }
    const GuestAddress header =
        table.resources + static_cast<GuestAddress>(textureNumber) * GUEST_RES_TIMG_BYTES;
    // The header's own address is its identity: a texture is the same texture to the renderer for
    // as long as the model that owns it is loaded, and two models' tables never overlap.
    textureError = native_render::decode_res_timg(source, native_render::ByteAddress::guest(header),
                                                  header, decoded);
    return textureError == native_render::ResTimgDecodeError::None
               ? GuestTextureError::None
               : GuestTextureError::DecodeFailed;
}

const char* name(GuestTextureSource source) noexcept {
    switch (source) {
    case GuestTextureSource::None:
        return "none";
    case GuestTextureSource::DisplayList:
        return "display list";
    case GuestTextureSource::Table:
        return "table";
    }
    return "unknown";
}

GuestTextureError
decode_guest_texmap_binding(const native_render::AssetByteSource& source,
                            const GuestTexmapBinding& binding,
                            native_render::DecodedTexture& decoded,
                            native_render::ResTimgDecodeError& textureError) noexcept {
    if (source.read == nullptr) {
        return GuestTextureError::NoReader;
    }
    if (!binding.bound()) {
        return GuestTextureError::TextureNumberOutOfRange;
    }

    // The list states a physical address; the same bytes are read through the guest's own view of
    // RAM, which is where every other reader here looks.
    const GuestAddress image = GUEST_RAM_BASE | binding.imageAddress;
    const GuestAddress palette =
        binding.paletteAddress != 0 ? (GUEST_RAM_BASE | binding.paletteAddress) : image;

    native_render::EncodedImageFormat format{};
    native_render::PaletteFormat paletteFormat{};
    if (!native_render::decode_image_format(binding.format, format)) {
        textureError = native_render::ResTimgDecodeError::UnsupportedFormat;
        return GuestTextureError::DecodeFailed;
    }
    if (!native_render::decode_palette_format(binding.paletteFormat, paletteFormat)) {
        textureError = native_render::ResTimgDecodeError::UnsupportedPalette;
        return GuestTextureError::DecodeFailed;
    }

    const native_render::ResTimgDescriptor descriptor{
        .format = binding.format,
        .hasAlpha = native_render::encoded_image_format_has_alpha(format, paletteFormat),
        .width = binding.width,
        .height = binding.height,
        .wrapS = binding.wrapS,
        .wrapT = binding.wrapT,
        .paletteFormat = binding.paletteFormat,
        .paletteEntries = binding.paletteEntries,
        .paletteOffset = static_cast<std::int32_t>(palette) - static_cast<std::int32_t>(image),
        .minFilter = binding.minFilter,
        .magFilter = binding.magFilter,
        .mipmapCount = binding.mipCount,
        .imageOffset = 0,
    };
    // The image's own address is its identity here, as the header's address is on the table path:
    // the list names no header, and two images loaded at once never share an address.
    textureError = native_render::decode_res_timg(
        source, descriptor, native_render::ByteAddress::guest(image), image, decoded);
    return textureError == native_render::ResTimgDecodeError::None
               ? GuestTextureError::None
               : GuestTextureError::DecodeFailed;
}

GuestTextureError decode_guest_material_texture(
    const native_render::AssetByteSource& source, const GuestDisplayListTextures& displayList,
    const GuestTextureTable& table, std::uint8_t textureMap, std::uint16_t textureNumber,
    native_render::DecodedTexture& decoded, native_render::ResTimgDecodeError& textureError,
    GuestTextureSource& resolvedFrom) noexcept {
    resolvedFrom = GuestTextureSource::None;
    if (textureMap < GUEST_MAX_TEXMAPS && displayList.texmap[textureMap].bound()) {
        resolvedFrom = GuestTextureSource::DisplayList;
        return decode_guest_texmap_binding(source, displayList.texmap[textureMap], decoded,
                                           textureError);
    }
    resolvedFrom = GuestTextureSource::Table;
    return decode_guest_texture(source, table, textureNumber, decoded, textureError);
}

} // namespace sb::title_adapter
