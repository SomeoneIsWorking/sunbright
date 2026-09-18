// Drives the shipping texture-generation reader against a synthetic guest image.
//
// There is only one thing to read here, which makes the negative cases the substance: an
// unrecognised block has to answer the value the shared classifiers already read as unsupported,
// and it has to say that it did rather than let a count of nothing look like a count of zero.

#include <sunbright/title_adapter/guest_j3d_texgen.h>

#include "guest_image.h"

#include <cassert>
#include <string_view>

namespace {

using sb::title_adapter::GuestAddress;
using sb::title_adapter::GuestMemory;
using sb::title_adapter::GuestTexGenBlock;
using sb::title_adapter::GuestTexGenBlockVtables;
using sb::title_adapter::GuestTexGenError;
using sb::title_adapter::kGuestUnsupportedTexGenCount;
using sb::title_adapter::test::Image;
using sb::title_adapter::test::read_image;

constexpr GuestAddress BASIC_VTABLE = 0x80000100;
constexpr GuestAddress BLOCK = 0x80000200;
constexpr GuestTexGenBlockVtables VTABLES{.basic = BASIC_VTABLE};

Image build_block(GuestAddress vtable, std::uint32_t count) {
    Image image;
    image.word(BLOCK + 0x00, vtable);
    image.word(BLOCK + 0x04, count);
    return image;
}

void reads_the_coordinate_count() {
    Image image = build_block(BASIC_VTABLE, 3);
    GuestMemory memory{read_image, &image};
    sb::native_render::J3dMaterialState state{};
    GuestTexGenBlock block{};
    assert(read_guest_tex_gen_block(memory, BLOCK, VTABLES, state, block) ==
           GuestTexGenError::None);
    assert(block.recognised);
    assert(block.blockType == static_cast<std::uint32_t>('TGBC'));
    assert(block.texGenCount == 3);
    assert(state.textureCoordinateCount == 3);
}

// A material may generate no coordinates at all, and zero is a count rather than an absence.
void reads_a_count_of_zero() {
    Image image = build_block(BASIC_VTABLE, 0);
    GuestMemory memory{read_image, &image};
    sb::native_render::J3dMaterialState state{};
    GuestTexGenBlock block{};
    assert(read_guest_tex_gen_block(memory, BLOCK, VTABLES, state, block) ==
           GuestTexGenError::None);
    assert(block.recognised);
    assert(block.texGenCount == 0);
    assert(state.textureCoordinateCount == 0);
}

// An unrecognised block is the decomp adapter's "not supported", not an error -- but the reader has
// to say so, or a block it never understood would be indistinguishable from one that generates
// nothing.
void marks_an_unrecognised_block_unsupported() {
    Image image = build_block(0x80000999, 3);
    GuestMemory memory{read_image, &image};
    sb::native_render::J3dMaterialState state{};
    GuestTexGenBlock block{};
    assert(read_guest_tex_gen_block(memory, BLOCK, VTABLES, state, block) ==
           GuestTexGenError::None);
    assert(!block.recognised);
    assert(block.blockType == 0);
    assert(block.texGenCount == kGuestUnsupportedTexGenCount);
    assert(state.textureCoordinateCount == kGuestUnsupportedTexGenCount);
}

void refuses_what_it_cannot_read() {
    Image image = build_block(BASIC_VTABLE, 3);
    sb::native_render::J3dMaterialState state{};
    GuestTexGenBlock block{};

    const GuestMemory none{nullptr, &image};
    assert(read_guest_tex_gen_block(none, BLOCK, VTABLES, state, block) ==
           GuestTexGenError::NoReader);

    GuestMemory memory{read_image, &image};
    assert(read_guest_tex_gen_block(memory, 0, VTABLES, state, block) ==
           GuestTexGenError::NullBlock);
    assert(read_guest_tex_gen_block(memory, 0x70000000, VTABLES, state, block) ==
           GuestTexGenError::UnreadableBlock);

    {
        // GX generates eight coordinates. A ninth is a read that landed elsewhere.
        Image tooMany = build_block(BASIC_VTABLE, 9);
        GuestMemory tooManyMemory{read_image, &tooMany};
        assert(read_guest_tex_gen_block(tooManyMemory, BLOCK, VTABLES, state, block) ==
               GuestTexGenError::TexGenCountOutOfRange);
    }
    {
        Image truncated = build_block(BASIC_VTABLE, 3);
        const GuestAddress edge =
            sb::title_adapter::test::RAM_BASE + sb::title_adapter::test::RAM_BYTES - 4;
        truncated.word(edge, BASIC_VTABLE);
        GuestMemory truncatedMemory{read_image, &truncated};
        assert(read_guest_tex_gen_block(truncatedMemory, edge, VTABLES, state, block) ==
               GuestTexGenError::UnreadableTexGenCount);
    }

    assert(state == sb::native_render::J3dMaterialState{});
}

void names_every_error() {
    for (std::uint8_t value = 0;
         value <= static_cast<std::uint8_t>(GuestTexGenError::TexGenCountOutOfRange); ++value) {
        const std::string_view text = name(static_cast<GuestTexGenError>(value));
        assert(!text.empty());
        assert(text != "unknown");
    }
}

} // namespace

int main() {
    reads_the_coordinate_count();
    reads_a_count_of_zero();
    marks_an_unrecognised_block_unsupported();
    refuses_what_it_cannot_read();
    names_every_error();
    return 0;
}
