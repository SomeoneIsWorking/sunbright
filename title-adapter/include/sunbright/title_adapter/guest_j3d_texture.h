#pragma once

#include <sunbright/native_render/res_timg_decode.h>
#include <sunbright/title_adapter/guest_memory.h>

#include <cstdint>

namespace sb::title_adapter {

// Resolves one of GMSE01's own textures and decodes it.
//
// A `J3DTexture` is the model's texture table: a count and an array of `ResTIMG` headers, which a
// TEV block's texture numbers index. Nothing here decodes anything itself --
// `native_render::decode_res_timg` already reads a big-endian `ResTIMG` from a `ByteAddress` and
// owns all eleven tiled formats, the three palette formats and the mip chain, and has done since
// before there was a guest path. This resolves the header's address and hands it over.
//
// The one layout fact that is not the decomp's is the table itself. `J3DTexture` in
// `decomp/sms` declares a virtual destructor, which would put the resource pointer at 0x08; the
// shipping image disagrees. `loadTexNo` (0x802eea04) reads `j3dSys.mTexture` from `0x54`, then
// `lwz r4, 4(r4)` and indexes it by the texture number shifted left five -- so the array pointer is
// at 0x04 and a `ResTIMG` is 0x20 bytes, both measured rather than assumed.

constexpr GuestAddress GUEST_TEXTURE_TABLE_COUNT = 0x00;
constexpr GuestAddress GUEST_TEXTURE_TABLE_RESOURCES = 0x04;
constexpr GuestAddress GUEST_RES_TIMG_BYTES = 0x20;

enum class GuestTextureError : std::uint8_t {
    None,
    NoReader,
    NullTable,
    UnreadableTable,
    EmptyTable,
    TextureNumberOutOfRange,
    NullResourceArray,
    DecodeFailed,
};

[[nodiscard]] const char* name(GuestTextureError error) noexcept;

struct GuestTextureTable {
    GuestAddress address = 0;
    std::uint16_t count = 0;
    GuestAddress resources = 0;
    // The halfword that follows the count. `mResourceCount` is a `u16` at 0x00 with the next two
    // bytes padding, so this is expected to be zero; it is carried out so a caller measuring
    // against the real title can see whether the count was read at the offset it thinks.
    std::uint16_t padding = 0;
};

[[nodiscard]] GuestTextureError read_guest_texture_table(const GuestMemory& memory,
                                                         GuestAddress table,
                                                         GuestTextureTable& out) noexcept;

// Decodes the numbered texture of `table`. `textureError` carries the decoder's own answer, which
// distinguishes a format this renderer does not own from a header that could not be read at all.
[[nodiscard]] GuestTextureError
decode_guest_texture(const native_render::AssetByteSource& source, const GuestTextureTable& table,
                     std::uint16_t textureNumber, native_render::DecodedTexture& decoded,
                     native_render::ResTimgDecodeError& textureError) noexcept;

} // namespace sb::title_adapter
