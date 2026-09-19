// Drives the shipping display-list reader over synthetic GX command streams.
//
// The reader's whole point is that it answers what a material binds rather than what its packet
// still points at, so the positive cases state the hardware fields exactly -- a size, a format, an
// address, a sampler, a mip count -- and the negative ones are what make those trustworthy. A list
// walked at the wrong address does not produce slightly wrong textures, it produces plausible ones,
// so an opcode the stream cannot contain has to refuse rather than resynchronise; a texmap given an
// address and never a format has to stay unbound rather than report a zero-by-zero image; and a
// colour-indexed texmap whose palette was never loaded has to say so instead of naming address 0.

#include <sunbright/title_adapter/guest_j3d_display_list.h>

#include "guest_image.h"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

using sb::title_adapter::GuestAddress;
using sb::title_adapter::GuestDisplayListError;
using sb::title_adapter::GuestDisplayListTextures;
using sb::title_adapter::GuestMemory;
using sb::title_adapter::read_guest_display_list_textures;
using sb::title_adapter::read_guest_material_packet_textures;
using sb::title_adapter::test::Image;
using sb::title_adapter::test::read_image;

constexpr GuestAddress LIST = 0x80000100;
constexpr GuestAddress PACKET = 0x80000040;
constexpr GuestAddress LIST_OBJECT = 0x80000080;

// The two registers GMSE01's sky actually writes, from the frame this reader was built for.
constexpr std::uint32_t SKY_IMAGE_ADDRESS = 0x00a84c00;
constexpr std::uint32_t CLOUD_IMAGE_ADDRESS = 0x00a84160;

// Builds a GX command stream. Nothing here knows the reader's opcode table; the bytes are the
// hardware's, written the way a display list carries them.
struct Stream {
    std::vector<std::uint8_t> bytes;

    void op(std::uint8_t opcode) { bytes.push_back(opcode); }
    void raw(std::uint8_t value) { bytes.push_back(value); }
    void word(std::uint32_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
        bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
        bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    void bp(std::uint8_t reg, std::uint32_t value) {
        op(0x61);
        word((static_cast<std::uint32_t>(reg) << 24U) | (value & 0x00FFFFFFU));
    }
    void loadXf(std::uint16_t address, const std::vector<std::uint32_t>& values) {
        op(0x10);
        const auto count = static_cast<std::uint16_t>(values.size() - 1);
        raw(static_cast<std::uint8_t>(count >> 8U));
        raw(static_cast<std::uint8_t>(count));
        raw(static_cast<std::uint8_t>(address >> 8U));
        raw(static_cast<std::uint8_t>(address));
        for (const std::uint32_t value : values) {
            word(value);
        }
    }

    void writeInto(Image& image, GuestAddress at) const {
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            image.byte(at + static_cast<GuestAddress>(index), bytes[index]);
        }
    }
};

constexpr std::uint32_t image0(std::uint16_t width, std::uint16_t height, std::uint8_t format) {
    return static_cast<std::uint32_t>(width - 1) | (static_cast<std::uint32_t>(height - 1) << 10U) |
           (static_cast<std::uint32_t>(format) << 20U);
}

// A texmap's whole binding, from a list that also carries the register loads a material really
// interleaves them with.
void reads_every_field_of_a_bound_texmap() {
    Stream stream;
    stream.loadXf(0x1009, {0x00000001, 0x00000002});
    // Hardware min filter 5 is GX's LIN_MIP_NEAR, which is 3 -- the orders differ, and a texture
    // whose four levels arrive as a filter that samples one would be refused as unsamplable.
    stream.bp(0x80, 1U | (2U << 2U) | (1U << 4U) | (5U << 5U));
    stream.bp(0x84, 48U << 8U);
    stream.bp(0x88, image0(256, 256, 0x0));
    stream.bp(0x94, SKY_IMAGE_ADDRESS >> 5U);
    stream.bp(0x89, image0(64, 64, 0xE));
    stream.bp(0x95, CLOUD_IMAGE_ADDRESS >> 5U);
    // The high block: texmap 4 is reached through a different register range, not a different rule.
    stream.bp(0xA8, image0(32, 16, 0x1));
    stream.bp(0xB4, 0x00ae0800U >> 5U);
    stream.op(0x00);

    Image image;
    stream.writeInto(image, LIST);
    GuestMemory memory{read_image, &image};
    GuestDisplayListTextures textures{};
    assert(read_guest_display_list_textures(memory, LIST,
                                            static_cast<std::uint32_t>(stream.bytes.size()),
                                            textures) == GuestDisplayListError::None);

    assert(textures.address == LIST);
    assert(textures.bytes == stream.bytes.size());
    assert(textures.commands == 10);
    assert(textures.registerWrites == 8);
    assert(textures.textureRegisterWrites == 8);
    assert(textures.boundCount() == 3);

    assert(textures.texmap[0].bound());
    assert(textures.texmap[0].width == 256);
    assert(textures.texmap[0].height == 256);
    assert(textures.texmap[0].format == 0x0);
    assert(textures.texmap[0].imageAddress == SKY_IMAGE_ADDRESS);
    assert(textures.texmap[0].wrapS == 1);
    assert(textures.texmap[0].wrapT == 2);
    assert(textures.texmap[0].magFilter == 1);
    assert(textures.texmap[0].minFilter == 3);
    assert(textures.texmap[0].mipCount == 4);

    assert(textures.texmap[1].bound());
    assert(textures.texmap[1].width == 64);
    assert(textures.texmap[1].format == 0xE);
    assert(textures.texmap[1].imageAddress == CLOUD_IMAGE_ADDRESS);
    // Untouched by this list, so it keeps the reader's defaults rather than texmap 0's sampler.
    assert(textures.texmap[1].mipCount == 1);

    assert(textures.texmap[4].bound());
    assert(textures.texmap[4].width == 32);
    assert(textures.texmap[4].height == 16);
    assert(textures.texmap[4].imageAddress == 0x00ae0800);

    for (const std::size_t empty :
         {std::size_t{2}, std::size_t{3}, std::size_t{5}, std::size_t{6}, std::size_t{7}}) {
        assert(!textures.texmap[empty].bound());
    }
}

// Half a binding is not a binding: a size with no address samples nothing, and an address with no
// size is an image of unknown extent. Either one reported as bound is a texture invented here.
// Every minification filter the hardware can encode, against the GX number the rest of the port --
// and every resource header -- states. The two undefined hardware encodings have to come out as
// values GX does not define, so a consumer refuses them instead of sampling something adjacent.
void translates_every_minification_filter() {
    constexpr std::uint8_t EXPECTED[8] = {0, 2, 4, 6, 1, 3, 5, 7};
    for (std::uint8_t hardware = 0; hardware < 8; ++hardware) {
        Stream stream;
        stream.bp(0x80, static_cast<std::uint32_t>(hardware) << 5U);
        stream.bp(0x88, image0(8, 8, 0x0));
        stream.bp(0x94, SKY_IMAGE_ADDRESS >> 5U);
        Image image;
        stream.writeInto(image, LIST);
        GuestMemory memory{read_image, &image};
        GuestDisplayListTextures textures{};
        assert(read_guest_display_list_textures(memory, LIST,
                                                static_cast<std::uint32_t>(stream.bytes.size()),
                                                textures) == GuestDisplayListError::None);
        assert(textures.texmap[0].minFilter == EXPECTED[hardware]);
    }
}

void refuses_to_call_half_a_binding_bound() {
    Stream addressOnly;
    addressOnly.bp(0x94, SKY_IMAGE_ADDRESS >> 5U);
    Stream sizeOnly;
    sizeOnly.bp(0x88, image0(64, 64, 0x0));

    for (const Stream& stream : {addressOnly, sizeOnly}) {
        Image image;
        stream.writeInto(image, LIST);
        GuestMemory memory{read_image, &image};
        GuestDisplayListTextures textures{};
        assert(read_guest_display_list_textures(memory, LIST,
                                                static_cast<std::uint32_t>(stream.bytes.size()),
                                                textures) == GuestDisplayListError::None);
        assert(textures.textureRegisterWrites == 1);
        assert(textures.boundCount() == 0);
        assert(!textures.texmap[0].bound());
    }
}

// A colour-indexed texmap is only decodable with the palette the list loaded, matched by the TMEM
// offset the texmap selected -- and an unmatched selection has to leave the palette absent.
void matches_a_palette_to_the_texmap_that_selected_it() {
    Stream stream;
    stream.bp(0x64, 0x00b00000U >> 5U);
    stream.bp(0x65, 0x30U | (16U << 10U));
    stream.bp(0x8A, image0(128, 128, 0x9));
    stream.bp(0x96, 0x00b10000U >> 5U);
    stream.bp(0x9A, 0x30U | (1U << 10U));
    stream.bp(0x8B, image0(32, 32, 0x9));
    stream.bp(0x97, 0x00b20000U >> 5U);
    stream.bp(0x9B, 0x7FU);

    Image image;
    stream.writeInto(image, LIST);
    GuestMemory memory{read_image, &image};
    GuestDisplayListTextures textures{};
    assert(read_guest_display_list_textures(memory, LIST,
                                            static_cast<std::uint32_t>(stream.bytes.size()),
                                            textures) == GuestDisplayListError::None);

    assert(textures.texmap[2].bound());
    assert(textures.texmap[2].paletteAddress == 0x00b00000);
    assert(textures.texmap[2].paletteEntries == 256);
    assert(textures.texmap[2].paletteFormat == 1);

    assert(textures.texmap[3].bound());
    assert(textures.texmap[3].paletteAddress == 0);
    assert(textures.texmap[3].paletteEntries == 0);
}

// A stream that is not a material display list, read as though it were, produces bindings that look
// like measurements. Every one of these refuses instead.
void refuses_what_it_cannot_walk() {
    Image image;
    GuestMemory memory{read_image, &image};
    GuestDisplayListTextures textures{};

    const GuestMemory none{nullptr, &image};
    assert(read_guest_display_list_textures(none, LIST, 4, textures) ==
           GuestDisplayListError::NoReader);
    assert(read_guest_display_list_textures(memory, 0, 4, textures) ==
           GuestDisplayListError::NullList);
    assert(read_guest_display_list_textures(memory, LIST, 0, textures) ==
           GuestDisplayListError::EmptyList);
    assert(read_guest_display_list_textures(memory, LIST, 1024U * 1024U, textures) ==
           GuestDisplayListError::ListTooLarge);
    assert(read_guest_display_list_textures(memory, 0x70000000, 32, textures) ==
           GuestDisplayListError::Unreadable);

    {
        Stream primitive;
        primitive.bp(0x88, image0(64, 64, 0x0));
        primitive.op(0x98);
        primitive.raw(0x00);
        primitive.raw(0x03);
        Image drawing;
        primitive.writeInto(drawing, LIST);
        GuestMemory drawingMemory{read_image, &drawing};
        assert(read_guest_display_list_textures(
                   drawingMemory, LIST, static_cast<std::uint32_t>(primitive.bytes.size()),
                   textures) == GuestDisplayListError::PrimitiveInMaterialList);
    }
    {
        Stream unknown;
        unknown.op(0x50);
        Image odd;
        unknown.writeInto(odd, LIST);
        GuestMemory oddMemory{read_image, &odd};
        assert(read_guest_display_list_textures(oddMemory, LIST, 1, textures) ==
               GuestDisplayListError::UnknownOpcode);
    }
    {
        Stream cut;
        cut.bp(0x88, image0(64, 64, 0x0));
        Image partial;
        cut.writeInto(partial, LIST);
        GuestMemory partialMemory{read_image, &partial};
        assert(read_guest_display_list_textures(partialMemory, LIST, 3, textures) ==
               GuestDisplayListError::Truncated);
    }
    {
        // An XF load whose word count runs past the end: the length arithmetic has to notice.
        Stream cut;
        cut.loadXf(0x1000, {1, 2, 3, 4});
        Image partial;
        cut.writeInto(partial, LIST);
        GuestMemory partialMemory{read_image, &partial};
        assert(read_guest_display_list_textures(partialMemory, LIST, 8, textures) ==
               GuestDisplayListError::Truncated);
    }
}

// The packet route, which is how a draw actually reaches its list.
void follows_a_material_packet_to_its_list() {
    Stream stream;
    stream.bp(0x88, image0(16, 8, 0x1));
    stream.bp(0x94, SKY_IMAGE_ADDRESS >> 5U);

    Image image;
    stream.writeInto(image, LIST);
    image.word(PACKET + 0x30, LIST_OBJECT);
    image.word(LIST_OBJECT + 0x00, LIST);
    image.word(LIST_OBJECT + 0x08, static_cast<std::uint32_t>(stream.bytes.size()));
    GuestMemory memory{read_image, &image};

    GuestDisplayListTextures textures{};
    assert(read_guest_material_packet_textures(memory, PACKET, textures) ==
           GuestDisplayListError::None);
    assert(textures.address == LIST);
    assert(textures.texmap[0].bound());
    assert(textures.texmap[0].width == 16);
    assert(textures.texmap[0].height == 8);

    assert(read_guest_material_packet_textures(memory, 0, textures) ==
           GuestDisplayListError::NullObject);
    assert(read_guest_material_packet_textures(memory, 0x70000000, textures) ==
           GuestDisplayListError::UnreadableObject);

    Image empty;
    empty.word(PACKET + 0x30, 0);
    GuestMemory emptyMemory{read_image, &empty};
    assert(read_guest_material_packet_textures(emptyMemory, PACKET, textures) ==
           GuestDisplayListError::NullObject);
}

} // namespace

int main() {
    reads_every_field_of_a_bound_texmap();
    translates_every_minification_filter();
    refuses_to_call_half_a_binding_bound();
    matches_a_palette_to_the_texmap_that_selected_it();
    refuses_what_it_cannot_walk();
    follows_a_material_packet_to_its_list();
    return 0;
}
