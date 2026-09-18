// Drives the shipping texture-table reader against a synthetic guest image.
//
// The decoding belongs to `native_render::decode_res_timg` and has its own tests; what is proved
// here is the resolution -- that the table's count and array pointer are read where the shipping
// image puts them, that a texture number the table does not hold is refused rather than indexed,
// and that the address handed to the decoder is the numbered header rather than the table.

#include <sunbright/title_adapter/guest_j3d_texture.h>

#include "guest_image.h"

#include <cassert>
#include <string_view>

namespace {

using sb::title_adapter::GuestAddress;
using sb::title_adapter::GuestMemory;
using sb::title_adapter::GuestTextureError;
using sb::title_adapter::GuestTextureTable;
using sb::title_adapter::test::Image;
using sb::title_adapter::test::read_image;

constexpr GuestAddress TABLE = 0x80000100;
constexpr GuestAddress RESOURCES = 0x80000200;
constexpr GuestAddress PIXELS = 0x80000400;

// One 4x4 RGB5A3 texture, the smallest complete resource the decoder accepts: format 5, one tile,
// no palette, no mip chain. The point is not the pixels but which header the decoder was given.
constexpr std::uint8_t RGB5A3 = 5;

void write_header(Image& image, GuestAddress header, std::uint16_t width, std::uint16_t height,
                  std::int32_t imageOffset) {
    image.byte(header + 0x00, RGB5A3);
    image.byte(header + 0x01, 1); // alphaEnabled
    image.half(header + 0x02, width);
    image.half(header + 0x04, height);
    image.byte(header + 0x06, 0); // wrapS = clamp
    image.byte(header + 0x07, 0); // wrapT = clamp
    image.byte(header + 0x14, 1); // minFilter = linear
    image.byte(header + 0x15, 1); // magFilter = linear
    image.byte(header + 0x18, 1); // mipmapCount
    image.word(header + 0x1c, static_cast<std::uint32_t>(imageOffset));
}

Image build_table(std::uint16_t count) {
    Image image;
    image.half(TABLE + 0x00, count);
    image.half(TABLE + 0x02, 0); // padding behind the count
    image.word(TABLE + 0x04, RESOURCES);
    // Two resources, each naming a different colour so the numbered one is identifiable.
    write_header(image, RESOURCES + 0x00, 4, 4,
                 static_cast<std::int32_t>(PIXELS - (RESOURCES + 0x00)));
    write_header(image, RESOURCES + 0x20, 4, 4,
                 static_cast<std::int32_t>((PIXELS + 0x20) - (RESOURCES + 0x20)));
    for (std::uint32_t texel = 0; texel < 16; ++texel) {
        image.half(PIXELS + texel * 2, 0xfc00);        // opaque red (RGB5A3 sets the top bit)
        image.half(PIXELS + 0x20 + texel * 2, 0x83e0); // opaque green
    }
    return image;
}

void reads_the_table() {
    Image image = build_table(2);
    GuestMemory memory{read_image, &image};
    GuestTextureTable table{};
    assert(read_guest_texture_table(memory, TABLE, table) == GuestTextureError::None);
    assert(table.address == TABLE);
    assert(table.count == 2);
    assert(table.resources == RESOURCES);
    assert(table.padding == 0);
}

// The header the decoder is given has to be the numbered one. Both resources decode successfully,
// so only their contents distinguish them -- which is exactly the mistake an off-by-one or a
// wrong stride makes.
void decodes_the_numbered_texture() {
    Image image = build_table(2);
    GuestMemory memory{read_image, &image};
    GuestTextureTable table{};
    assert(read_guest_texture_table(memory, TABLE, table) == GuestTextureError::None);

    const sb::native_render::AssetByteSource source{
        [](sb::native_render::ByteAddress address, std::span<std::uint8_t> output,
           void* context) -> bool {
            std::uint64_t guestAddress = 0;
            if (!address.guest_value(guestAddress)) {
                return false;
            }
            return read_image(static_cast<GuestAddress>(guestAddress), output, context);
        },
        &image};

    sb::native_render::DecodedTexture first{};
    sb::native_render::ResTimgDecodeError error = sb::native_render::ResTimgDecodeError::None;
    assert(decode_guest_texture(source, table, 0, first, error) == GuestTextureError::None);
    assert(error == sb::native_render::ResTimgDecodeError::None);
    assert(first.texture.width == 4);
    assert(first.texture.height == 4);
    assert(first.rgba8.size() == 4 * 4 * 4);
    assert(first.rgba8[0] == 0xFF); // red
    assert(first.rgba8[1] == 0x00);

    sb::native_render::DecodedTexture second{};
    assert(decode_guest_texture(source, table, 1, second, error) == GuestTextureError::None);
    assert(second.rgba8[0] == 0x00);
    assert(second.rgba8[1] == 0xFF); // green
    // Two textures of one model are two resources, and the renderer has to be able to tell them
    // apart even when their headers are otherwise identical.
    assert(first.texture.resource != second.texture.resource);
}

void refuses_what_it_cannot_read() {
    Image image = build_table(2);
    GuestMemory memory{read_image, &image};
    GuestTextureTable table{};

    const GuestMemory none{nullptr, &image};
    assert(read_guest_texture_table(none, TABLE, table) == GuestTextureError::NoReader);
    assert(read_guest_texture_table(memory, 0, table) == GuestTextureError::NullTable);
    assert(read_guest_texture_table(memory, 0x70000000, table) ==
           GuestTextureError::UnreadableTable);
    {
        Image empty = build_table(0);
        GuestMemory emptyMemory{read_image, &empty};
        assert(read_guest_texture_table(emptyMemory, TABLE, table) ==
               GuestTextureError::EmptyTable);
    }
    {
        Image headless = build_table(2);
        headless.word(TABLE + 0x04, 0);
        GuestMemory headlessMemory{read_image, &headless};
        assert(read_guest_texture_table(headlessMemory, TABLE, table) ==
               GuestTextureError::NullResourceArray);
    }

    assert(read_guest_texture_table(memory, TABLE, table) == GuestTextureError::None);
    const sb::native_render::AssetByteSource source{
        [](sb::native_render::ByteAddress address, std::span<std::uint8_t> output,
           void* context) -> bool {
            std::uint64_t guestAddress = 0;
            if (!address.guest_value(guestAddress)) {
                return false;
            }
            return read_image(static_cast<GuestAddress>(guestAddress), output, context);
        },
        &image};
    sb::native_render::DecodedTexture decoded{};
    sb::native_render::ResTimgDecodeError error = sb::native_render::ResTimgDecodeError::None;
    // A texture number the table does not hold is a misread binding, not a texture past the end.
    assert(decode_guest_texture(source, table, 2, decoded, error) ==
           GuestTextureError::TextureNumberOutOfRange);
    const sb::native_render::AssetByteSource noSource{};
    assert(decode_guest_texture(noSource, table, 0, decoded, error) == GuestTextureError::NoReader);
    {
        // A header the source cannot read is a failure the decoder reports, and it has to arrive
        // as one rather than as an empty texture.
        GuestTextureTable unmapped = table;
        unmapped.resources = 0x70000000;
        assert(decode_guest_texture(source, unmapped, 0, decoded, error) ==
               GuestTextureError::DecodeFailed);
        assert(error != sb::native_render::ResTimgDecodeError::None);
    }
}

void names_every_error() {
    for (std::uint8_t value = 0;
         value <= static_cast<std::uint8_t>(GuestTextureError::DecodeFailed); ++value) {
        const std::string_view text = name(static_cast<GuestTextureError>(value));
        assert(!text.empty());
        assert(text != "unknown");
    }
}

} // namespace

int main() {
    reads_the_table();
    decodes_the_numbered_texture();
    refuses_what_it_cannot_read();
    names_every_error();
    return 0;
}
