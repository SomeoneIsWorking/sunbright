// Drives the shipping colour-stage reader against a synthetic guest image.
//
// The four block classes share their first field and nothing else, and every one of them is a
// plausible reading of the others: a 'TVB4' block read as a 'TVB2' finds a stage count, stages and
// colours, all at the wrong offsets and all looking like a material. The fixtures therefore write
// a value that belongs to exactly one field of one class, so a layout confusion lands somewhere the
// assertions name rather than on a neighbour's plausible value.

#include <sunbright/title_adapter/guest_j3d_tev.h>

#include "guest_image.h"

#include <array>
#include <cassert>
#include <string_view>

namespace {

using sb::title_adapter::GuestAddress;
using sb::title_adapter::GuestMemory;
using sb::title_adapter::GuestTevBlock;
using sb::title_adapter::GuestTevBlockKind;
using sb::title_adapter::GuestTevBlockVtables;
using sb::title_adapter::GuestTevError;
using sb::title_adapter::test::Image;
using sb::title_adapter::test::read_image;

constexpr GuestAddress VTABLE_1 = 0x80000100;
constexpr GuestAddress VTABLE_2 = 0x80000110;
constexpr GuestAddress VTABLE_4 = 0x80000120;
constexpr GuestAddress VTABLE_16 = 0x80000130;
constexpr GuestAddress BLOCK = 0x80000200;

constexpr GuestTevBlockVtables VTABLES{
    .stage1 = VTABLE_1, .stage2 = VTABLE_2, .stage4 = VTABLE_4, .stage16 = VTABLE_16};

void write_order(Image& image, GuestAddress address, std::uint8_t coordinate, std::uint8_t map,
                 std::uint8_t channel) {
    image.byte(address + 0, coordinate);
    image.byte(address + 1, map);
    image.byte(address + 2, channel);
}

void write_stage(Image& image, GuestAddress address, std::uint8_t seed) {
    for (std::uint8_t byte = 0; byte < 8; ++byte) {
        image.byte(address + byte, static_cast<std::uint8_t>(seed + byte));
    }
}

// J3DTevBlock2: texture numbers 0x04, orders 0x08, register colours 0x10, stage count 0x30, stages
// 0x31, constant colours 0x41, constant selections 0x51 and 0x53.
Image build_stage2(std::uint8_t stageCount) {
    Image image;
    image.word(BLOCK + 0x00, VTABLE_2);
    image.half(BLOCK + 0x04, 0x0111);
    image.half(BLOCK + 0x06, 0x0222);
    write_order(image, BLOCK + 0x08, 1, 2, 3);
    write_order(image, BLOCK + 0x0c, 4, 5, 6);
    // Four s10 register colours; only the first three reach the shared state.
    for (std::uint16_t colour = 0; colour < 4; ++colour) {
        const GuestAddress base = BLOCK + 0x10 + colour * 8;
        image.half(base + 0, static_cast<std::uint16_t>(0x0100 + colour));
        image.half(base + 2, static_cast<std::uint16_t>(0x0200 + colour));
        image.half(base + 4, static_cast<std::uint16_t>(0x0300 + colour));
        // A negative component, which is what an s10 register colour is for.
        image.half(base + 6, static_cast<std::uint16_t>(0xfff0 + colour));
    }
    image.byte(BLOCK + 0x30, stageCount);
    write_stage(image, BLOCK + 0x31, 0x40);
    write_stage(image, BLOCK + 0x39, 0x60);
    for (std::uint8_t colour = 0; colour < 4; ++colour) {
        const GuestAddress base = BLOCK + 0x41 + colour * 4;
        image.byte(base + 0, static_cast<std::uint8_t>(0x10 + colour));
        image.byte(base + 1, static_cast<std::uint8_t>(0x20 + colour));
        image.byte(base + 2, static_cast<std::uint8_t>(0x30 + colour));
        image.byte(base + 3, static_cast<std::uint8_t>(0x40 + colour));
    }
    image.byte(BLOCK + 0x51, 0x0a); // konst colour selection, stage 0
    image.byte(BLOCK + 0x52, 0x0b); // stage 1
    image.byte(BLOCK + 0x53, 0x0c); // konst alpha selection, stage 0
    image.byte(BLOCK + 0x54, 0x0d); // stage 1
    return image;
}

void reads_a_two_stage_block() {
    Image image = build_stage2(2);
    GuestMemory memory{read_image, &image};
    sb::native_render::J3dMaterialState state{};
    GuestTevBlock block{};
    assert(read_guest_tev_block(memory, BLOCK, VTABLES, state, block) == GuestTevError::None);

    assert(block.kind == GuestTevBlockKind::Stage2);
    assert(block.blockType == static_cast<std::uint32_t>('TVB2'));
    assert(block.stageCount == 2);
    assert(block.stageCapacity == 2);
    assert(block.textureBindingCount == 2);

    assert(state.tevBlockType == static_cast<std::uint32_t>('TVB2'));
    assert(state.supportedTevBlock);
    assert(state.tevStageCount == 2);
    assert(state.textureBindings[0].textureNumber == 0x0111);
    assert(state.textureBindings[1].textureNumber == 0x0222);
    // Past the block's own binding count nothing is bound, and 0xFFFF is what the shared state
    // spells that as -- not texture zero.
    assert(state.textureBindings[2].textureNumber == 0xFFFF);

    assert(state.tevStages[0].textureCoordinate == 1);
    assert(state.tevStages[0].textureMap == 2);
    assert(state.tevStages[0].colorChannel == 3);
    assert(state.tevStages[1].textureCoordinate == 4);
    assert(state.tevStages[1].textureMap == 5);
    assert(state.tevStages[1].colorChannel == 6);
    for (std::uint8_t byte = 0; byte < 8; ++byte) {
        assert(state.tevStages[0].program[byte] == 0x40 + byte);
        assert(state.tevStages[1].program[byte] == 0x60 + byte);
    }

    assert(state.hasTevColors);
    for (std::size_t colour = 0; colour < state.tevColorsS10.size(); ++colour) {
        assert(state.tevColorsS10[colour][0] == static_cast<std::int16_t>(0x0100 + colour));
        assert(state.tevColorsS10[colour][1] == static_cast<std::int16_t>(0x0200 + colour));
        assert(state.tevColorsS10[colour][2] == static_cast<std::int16_t>(0x0300 + colour));
        // An s10 component is signed, so this has to come back negative rather than as 0xfff0.
        assert(state.tevColorsS10[colour][3] ==
               static_cast<std::int16_t>(static_cast<std::uint16_t>(0xfff0 + colour)));
        assert(state.tevColorsS10[colour][3] < 0);
    }
    assert(state.konstColorRgba8[0] == 0x10203040U);
    assert(state.konstColorRgba8[3] == 0x13233343U);
    assert(state.tevStages[0].konstColorSelection == 0x0a);
    assert(state.tevStages[1].konstColorSelection == 0x0b);
    assert(state.tevStages[0].konstAlphaSelection == 0x0c);
    assert(state.tevStages[1].konstAlphaSelection == 0x0d);
}

// A stage past the active count is not read as a program, but its constant selections still are:
// that is what the decomp adapter does, and the difference shows in exactly this case.
void reads_one_stage_of_a_two_stage_block() {
    Image image = build_stage2(1);
    GuestMemory memory{read_image, &image};
    sb::native_render::J3dMaterialState state{};
    GuestTevBlock block{};
    assert(read_guest_tev_block(memory, BLOCK, VTABLES, state, block) == GuestTevError::None);
    assert(state.tevStageCount == 1);
    assert(state.tevStages[0].program[0] == 0x40);
    assert(state.tevStages[1].program[0] == 0);
    assert(state.tevStages[1].textureMap == 0);
    assert(state.tevStages[1].konstColorSelection == 0x0b);
    assert(state.tevStages[1].konstAlphaSelection == 0x0d);
}

// J3DTevBlock1 carries no register or constant colours at all. The decomp adapter's capture fails
// on the first null it asks for, so a 'TVB1' material reaches the classifiers from neither runtime.
void refuses_a_block_with_no_colours() {
    Image image;
    image.word(BLOCK + 0x00, VTABLE_1);
    image.half(BLOCK + 0x04, 0x0333);
    write_order(image, BLOCK + 0x06, 7, 1, 4);
    write_stage(image, BLOCK + 0x0a, 0x80);
    GuestMemory memory{read_image, &image};
    sb::native_render::J3dMaterialState state{};
    GuestTevBlock block{};
    assert(read_guest_tev_block(memory, BLOCK, VTABLES, state, block) ==
           GuestTevError::BlockHasNoTevColors);
    assert(state == sb::native_render::J3dMaterialState{});
}

// The sixteen-stage block is the one whose offsets are furthest from every other class, and the
// only one whose stage index reaches past a byte of orders.
void reads_a_sixteen_stage_block() {
    Image image;
    image.word(BLOCK + 0x000, VTABLE_16);
    for (std::uint8_t binding = 0; binding < 8; ++binding) {
        image.half(BLOCK + 0x004 + binding * 2, static_cast<std::uint16_t>(0x0500 + binding));
    }
    for (std::uint8_t stage = 0; stage < 16; ++stage) {
        write_order(image, BLOCK + 0x014 + stage * 4, stage, static_cast<std::uint8_t>(stage + 1),
                    static_cast<std::uint8_t>(stage + 2));
    }
    image.byte(BLOCK + 0x054, 16);
    for (std::uint8_t stage = 0; stage < 16; ++stage) {
        write_stage(image, BLOCK + 0x055 + stage * 8, static_cast<std::uint8_t>(stage * 8));
    }
    for (std::uint16_t colour = 0; colour < 4; ++colour) {
        const GuestAddress base = BLOCK + 0x0d6 + colour * 8;
        image.half(base + 0, static_cast<std::uint16_t>(0x0700 + colour));
        image.half(base + 2, 0);
        image.half(base + 4, 0);
        image.half(base + 6, 0);
    }
    for (std::uint8_t colour = 0; colour < 4; ++colour) {
        image.byte(BLOCK + 0x0f6 + colour * 4, static_cast<std::uint8_t>(0xa0 + colour));
    }
    for (std::uint8_t stage = 0; stage < 16; ++stage) {
        image.byte(BLOCK + 0x106 + stage, static_cast<std::uint8_t>(0xc0 + stage));
        image.byte(BLOCK + 0x116 + stage, static_cast<std::uint8_t>(0xe0 + stage));
    }

    GuestMemory memory{read_image, &image};
    sb::native_render::J3dMaterialState state{};
    GuestTevBlock block{};
    assert(read_guest_tev_block(memory, BLOCK, VTABLES, state, block) == GuestTevError::None);
    assert(block.kind == GuestTevBlockKind::Stage16);
    assert(block.blockType == static_cast<std::uint32_t>('TV16'));
    assert(block.stageCount == 16);
    assert(block.textureBindingCount == 8);
    assert(state.textureBindings[7].textureNumber == 0x0507);
    assert(state.tevStageCount == 16);
    assert(state.tevStages[15].textureCoordinate == 15);
    assert(state.tevStages[15].textureMap == 16);
    assert(state.tevStages[15].colorChannel == 17);
    assert(state.tevStages[15].program[0] == 15 * 8);
    assert(state.tevStages[15].konstColorSelection == 0xc0 + 15);
    assert(state.tevStages[15].konstAlphaSelection == 0xe0 + 15);
    assert(state.tevColorsS10[2][0] == 0x0702);
    assert((state.konstColorRgba8[3] >> 24U) == 0xa3U);
}

void leaves_the_other_blocks_alone() {
    Image image = build_stage2(2);
    GuestMemory memory{read_image, &image};
    sb::native_render::J3dMaterialState state{};
    state.cullMode = 2;
    state.pixelEngineBlockType = static_cast<std::uint32_t>('PEFL');
    state.materialColorRgba8 = 0xaabbccddU;
    GuestTevBlock block{};
    assert(read_guest_tev_block(memory, BLOCK, VTABLES, state, block) == GuestTevError::None);
    assert(state.cullMode == 2);
    assert(state.pixelEngineBlockType == static_cast<std::uint32_t>('PEFL'));
    assert(state.materialColorRgba8 == 0xaabbccddU);
}

void refuses_what_it_cannot_read() {
    Image image = build_stage2(2);
    sb::native_render::J3dMaterialState state{};
    GuestTevBlock block{};

    const GuestMemory none{nullptr, &image};
    assert(read_guest_tev_block(none, BLOCK, VTABLES, state, block) == GuestTevError::NoReader);

    GuestMemory memory{read_image, &image};
    assert(read_guest_tev_block(memory, 0, VTABLES, state, block) == GuestTevError::NullBlock);
    assert(read_guest_tev_block(memory, 0x70000000, VTABLES, state, block) ==
           GuestTevError::UnreadableBlock);

    {
        Image unknown = build_stage2(2);
        unknown.word(BLOCK + 0x00, 0x80000999);
        GuestMemory unknownMemory{read_image, &unknown};
        assert(read_guest_tev_block(unknownMemory, BLOCK, VTABLES, state, block) ==
               GuestTevError::UnknownBlockKind);
    }
    {
        // A 'TVB2' block has storage for two stages. A third is a read that landed elsewhere, and
        // answering it would index past the block's own array into the colours behind it.
        Image tooMany = build_stage2(3);
        GuestMemory tooManyMemory{read_image, &tooMany};
        assert(read_guest_tev_block(tooManyMemory, BLOCK, VTABLES, state, block) ==
               GuestTevError::StageCountPastBlockCapacity);
    }
    {
        Image truncated = build_stage2(2);
        const GuestAddress edge =
            sb::title_adapter::test::RAM_BASE + sb::title_adapter::test::RAM_BYTES - 0x10;
        truncated.word(edge, VTABLE_2);
        GuestMemory truncatedMemory{read_image, &truncated};
        assert(read_guest_tev_block(truncatedMemory, edge, VTABLES, state, block) ==
               GuestTevError::UnreadableStageCount);
    }

    assert(state == sb::native_render::J3dMaterialState{});
}

void agrees_with_the_decomp_block_codes() {
    using sb::title_adapter::guest_tev_block_type;
    assert(guest_tev_block_type(GuestTevBlockKind::Stage1) == static_cast<std::uint32_t>('TVB1'));
    assert(guest_tev_block_type(GuestTevBlockKind::Stage2) == static_cast<std::uint32_t>('TVB2'));
    assert(guest_tev_block_type(GuestTevBlockKind::Stage4) == static_cast<std::uint32_t>('TVB4'));
    assert(guest_tev_block_type(GuestTevBlockKind::Stage16) == static_cast<std::uint32_t>('TV16'));
}

void names_every_error() {
    for (std::uint8_t value = 0;
         value <= static_cast<std::uint8_t>(GuestTevError::UnreadableKonstSelection); ++value) {
        const std::string_view text = name(static_cast<GuestTevError>(value));
        assert(!text.empty());
        assert(text != "unknown");
    }
}

} // namespace

int main() {
    reads_a_two_stage_block();
    reads_one_stage_of_a_two_stage_block();
    refuses_a_block_with_no_colours();
    reads_a_sixteen_stage_block();
    leaves_the_other_blocks_alone();
    refuses_what_it_cannot_read();
    agrees_with_the_decomp_block_codes();
    names_every_error();
    return 0;
}
