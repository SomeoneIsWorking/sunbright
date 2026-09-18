#include <sunbright/title_adapter/guest_j3d_texture.h>

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

} // namespace sb::title_adapter
