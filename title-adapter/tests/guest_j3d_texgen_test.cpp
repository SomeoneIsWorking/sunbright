// Drives the shipping texture-generation reader against a synthetic guest image.
//
// The block says how many coordinates a material generates, what each one reads, and which matrix
// transforms it. The negative cases are as much of the substance as the positive ones: an
// unrecognised block has to answer the value the shared classifiers already read as unsupported,
// and it has to say that it did rather than let a count of nothing look like a count of zero; and a
// matrix slot a title left empty has to stay distinguishable from one holding the identity.

#include <sunbright/title_adapter/guest_j3d_texgen.h>

#include "guest_image.h"

#include <cassert>
#include <string_view>

namespace {

using sb::title_adapter::guest_tex_gen_matrix_slot;
using sb::title_adapter::GuestAddress;
using sb::title_adapter::GuestMemory;
using sb::title_adapter::GuestTexCoordDefinition;
using sb::title_adapter::GuestTexGenBlock;
using sb::title_adapter::GuestTexGenBlockVtables;
using sb::title_adapter::GuestTexGenError;
using sb::title_adapter::kGuestMaxTexMatrixCount;
using sb::title_adapter::kGuestTexGenMatrixIdentity;
using sb::title_adapter::kGuestUnsupportedTexGenCount;
using sb::title_adapter::test::Image;
using sb::title_adapter::test::read_image;

constexpr GuestAddress BASIC_VTABLE = 0x80000100;
constexpr GuestAddress BLOCK = 0x80000200;
constexpr GuestAddress MATRIX = 0x80000400;
constexpr GuestTexGenBlockVtables VTABLES{.basic = BASIC_VTABLE};

Image build_block(GuestAddress vtable, std::uint32_t count) {
    Image image;
    image.word(BLOCK + 0x00, vtable);
    image.word(BLOCK + 0x04, count);
    return image;
}

// The generators and their matrices, which is what the block is read for: a coordinate that names
// a matrix carries the one the title computed, and one that names the identity selector carries no
// matrix rather than an identity it never authored.
void reads_each_coordinate_and_its_matrix() {
    Image image = build_block(BASIC_VTABLE, 2);
    image.byte(BLOCK + 0x08, 1);
    image.byte(BLOCK + 0x09, 4);
    image.byte(BLOCK + 0x0a, 30);
    image.byte(BLOCK + 0x0c, 0);
    image.byte(BLOCK + 0x0d, 5);
    image.byte(BLOCK + 0x0e, kGuestTexGenMatrixIdentity);
    // A third entry the block does not generate, to prove the reader stops at the count.
    image.byte(BLOCK + 0x10, 9);
    image.byte(BLOCK + 0x11, 9);
    image.byte(BLOCK + 0x12, 9);
    image.word(BLOCK + 0x28, MATRIX);
    image.byte(MATRIX + 0x00, 1);
    image.byte(MATRIX + 0x01, 2);
    for (std::uint32_t element = 0; element < 12; ++element) {
        image.real(MATRIX + 0x64 + (element * 4), static_cast<float>(element) + 0.5F);
    }

    GuestMemory memory{read_image, &image};
    sb::native_render::J3dMaterialState state{};
    GuestTexGenBlock block{};
    assert(read_guest_tex_gen_block(memory, BLOCK, VTABLES, state, block) ==
           GuestTexGenError::None);
    assert(block.texGenCount == 2);
    assert(block.coordinates[0] ==
           (GuestTexCoordDefinition{.texGenType = 1, .texGenSrc = 4, .texGenMatrix = 30}));
    assert(block.coordinates[1] ==
           (GuestTexCoordDefinition{
               .texGenType = 0, .texGenSrc = 5, .texGenMatrix = kGuestTexGenMatrixIdentity}));
    assert(block.coordinates[2] == GuestTexCoordDefinition{});
    assert(block.matrices[0].present);
    assert(block.matrices[0].projection == 1);
    assert(block.matrices[0].info == 2);
    for (std::uint32_t element = 0; element < 12; ++element) {
        assert(block.matrices[0].total[element] == static_cast<float>(element) + 0.5F);
    }
    for (std::uint32_t slot = 1; slot < kGuestMaxTexMatrixCount; ++slot) {
        assert(!block.matrices[slot].present);
    }

    // The neutral description the coordinate step takes says the same thing without the guest's
    // numbering: the first coordinate carries the matrix, the second carries none.
    const sb::native_render::J3dTexCoordGeneration generation =
        build_guest_tex_coord_generation(block);
    assert(generation.count == 2);
    assert(generation.generators[0].hasMatrix);
    assert(generation.generators[0].matrix == block.matrices[0].total);
    assert(generation.generators[0].type == sb::native_render::J3dTexGenType::Matrix2x4);
    assert(generation.generators[0].source == 4);
    assert(!generation.generators[1].hasMatrix);
    assert(generation.generators[1].type == sb::native_render::J3dTexGenType::Matrix3x4);
}

// Which slot a matrix identifier names. The nine loadable matrices are three apart, and everything
// else -- the identity selector, a value between two of them, a value past the last -- names no
// slot at all.
void maps_matrix_identifiers_to_slots() {
    for (std::uint32_t slot = 0; slot < kGuestMaxTexMatrixCount; ++slot) {
        assert(guest_tex_gen_matrix_slot(static_cast<std::uint8_t>(30 + (slot * 3))) == slot);
    }
    assert(guest_tex_gen_matrix_slot(kGuestTexGenMatrixIdentity) == kGuestMaxTexMatrixCount);
    assert(guest_tex_gen_matrix_slot(31) == kGuestMaxTexMatrixCount);
    assert(guest_tex_gen_matrix_slot(29) == kGuestMaxTexMatrixCount);
    assert(guest_tex_gen_matrix_slot(0) == kGuestMaxTexMatrixCount);
    // The ninth loadable matrix has no slot in a block that holds eight.
    assert(guest_tex_gen_matrix_slot(30 + (8 * 3)) == kGuestMaxTexMatrixCount);
}

// A block whose matrix pointer leads somewhere unreadable is a failure, not an empty slot.
void refuses_a_matrix_it_cannot_read() {
    Image image = build_block(BASIC_VTABLE, 1);
    image.byte(BLOCK + 0x08, 1);
    image.byte(BLOCK + 0x09, 4);
    image.byte(BLOCK + 0x0a, 30);
    image.word(BLOCK + 0x28, 0x70000000);
    GuestMemory memory{read_image, &image};
    sb::native_render::J3dMaterialState state{};
    GuestTexGenBlock block{};
    assert(read_guest_tex_gen_block(memory, BLOCK, VTABLES, state, block) ==
           GuestTexGenError::UnreadableTexMatrix);
    assert(state == sb::native_render::J3dMaterialState{});
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
         value <= static_cast<std::uint8_t>(GuestTexGenError::UnreadableTexMatrix); ++value) {
        const std::string_view text = name(static_cast<GuestTexGenError>(value));
        assert(!text.empty());
        assert(text != "unknown");
    }
}

} // namespace

int main() {
    reads_the_coordinate_count();
    reads_each_coordinate_and_its_matrix();
    maps_matrix_identifiers_to_slots();
    refuses_a_matrix_it_cannot_read();
    reads_a_count_of_zero();
    marks_an_unrecognised_block_unsupported();
    refuses_what_it_cannot_read();
    names_every_error();
    return 0;
}
