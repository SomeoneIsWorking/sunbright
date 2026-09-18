// Drives the shipping colour-block reader against a synthetic guest image.
//
// The two block classes are the thing most easily got wrong here: they share their first field and
// nothing after it, so a reader that picked the wrong layout still finds a plausible cull mode, a
// plausible channel count and plausible colours -- just the wrong ones. Both fixtures are built in
// the same image, at the same address, with values that belong to no other field, so reading one
// with the other's offsets lands somewhere the assertions name.

#include <sunbright/title_adapter/guest_j3d_color.h>

#include "guest_image.h"

#include <array>
#include <cassert>
#include <string_view>

namespace {

using sb::title_adapter::GuestAddress;
using sb::title_adapter::GuestColorBlock;
using sb::title_adapter::GuestColorBlockKind;
using sb::title_adapter::GuestColorBlockVtables;
using sb::title_adapter::GuestColorError;
using sb::title_adapter::GuestMemory;
using sb::title_adapter::test::Image;
using sb::title_adapter::test::read_image;

constexpr GuestAddress LIGHT_OFF_VTABLE = 0x80000100;
constexpr GuestAddress LIGHT_ON_VTABLE = 0x80000110;
constexpr GuestAddress BLOCK = 0x80000200;

constexpr GuestColorBlockVtables VTABLES{.lightOff = LIGHT_OFF_VTABLE, .lightOn = LIGHT_ON_VTABLE};

// GX_CC_ENABLE, the bit the block's own `load()` and the shared classifiers both read as "this
// channel is lit".
constexpr std::uint16_t LIGHTING = 0x0002;

Image build_light_off(std::uint8_t channelCount, std::uint16_t colorControl) {
    Image image;
    image.word(BLOCK + 0x00, LIGHT_OFF_VTABLE);
    image.byte(BLOCK + 0x04, 0x11); // mMatColor[0]
    image.byte(BLOCK + 0x05, 0x22);
    image.byte(BLOCK + 0x06, 0x33);
    image.byte(BLOCK + 0x07, 0x44);
    image.byte(BLOCK + 0x08, 0x55); // mMatColor[1]
    image.byte(BLOCK + 0x09, 0x66);
    image.byte(BLOCK + 0x0a, 0x77);
    image.byte(BLOCK + 0x0b, 0x88);
    image.byte(BLOCK + 0x0c, channelCount);
    image.half(BLOCK + 0x0e, colorControl); // mColorChan[0]
    image.half(BLOCK + 0x10, 0x0a51);       // mColorChan[1]
    image.half(BLOCK + 0x12, 0x0b52);       // mColorChan[2]
    image.half(BLOCK + 0x14, 0x0c53);       // mColorChan[3]
    image.byte(BLOCK + 0x16, 2);            // mCullMode
    return image;
}

Image build_light_on(std::uint8_t channelCount, std::uint16_t colorControl) {
    Image image;
    image.word(BLOCK + 0x00, LIGHT_ON_VTABLE);
    image.byte(BLOCK + 0x04, 0x11); // mMatColor[0]
    image.byte(BLOCK + 0x05, 0x22);
    image.byte(BLOCK + 0x06, 0x33);
    image.byte(BLOCK + 0x07, 0x44);
    image.byte(BLOCK + 0x08, 0x55); // mMatColor[1]
    image.byte(BLOCK + 0x09, 0x66);
    image.byte(BLOCK + 0x0a, 0x77);
    image.byte(BLOCK + 0x0b, 0x88);
    image.byte(BLOCK + 0x0c, 0x99); // mAmbColor[0]
    image.byte(BLOCK + 0x0d, 0xaa);
    image.byte(BLOCK + 0x0e, 0xbb);
    image.byte(BLOCK + 0x0f, 0xcc);
    image.byte(BLOCK + 0x10, 0xdd); // mAmbColor[1]
    image.byte(BLOCK + 0x11, 0xee);
    image.byte(BLOCK + 0x12, 0x0f);
    image.byte(BLOCK + 0x13, 0x10);
    image.byte(BLOCK + 0x14, channelCount);
    image.half(BLOCK + 0x16, colorControl); // mColorChan[0]
    image.half(BLOCK + 0x18, 0x0a51);       // mColorChan[1]
    image.half(BLOCK + 0x1a, 0x0b52);       // mColorChan[2]
    image.half(BLOCK + 0x1c, 0x0c53);       // mColorChan[3]
    image.byte(BLOCK + 0x40, 3);            // mCullMode
    return image;
}

void reads_an_unlit_block() {
    Image image = build_light_off(2, 0x0940);
    GuestMemory memory{read_image, &image};
    sb::native_render::J3dMaterialState state{};
    GuestColorBlock block{};
    assert(read_guest_color_block(memory, BLOCK, VTABLES, state, block) == GuestColorError::None);

    assert(block.kind == GuestColorBlockKind::LightOff);
    assert(block.blockType == static_cast<std::uint32_t>('CLOF'));
    assert(block.colorChannelCount == 2);
    assert(!block.lightingEnabled);

    assert(state.supportedColorBlock);
    assert(!state.usesMaterialAmbient);
    assert(state.cullMode == 2);
    assert(state.colorChannelCount == 2);
    assert(state.colorChannelControl == 0x0940);
    assert(state.alphaChannelControl == 0x0a51);
    assert(state.colorChannelControl1 == 0x0b52);
    assert(state.alphaChannelControl1 == 0x0c53);
    assert(!state.lightingEnabled);
    assert(state.materialColorRgba8 == 0x11223344U);
    assert(state.materialColor1Rgba8 == 0x55667788U);
    // J3DColorBlockLightOff has no ambient colours at all, so there is nothing to read and nothing
    // that could be read by accident from the fields that follow.
    assert(state.ambientColorRgba8 == 0);
    assert(state.ambientColor1Rgba8 == 0);
}

void reads_a_lit_block() {
    Image image = build_light_on(2, static_cast<std::uint16_t>(0x0940 | LIGHTING));
    GuestMemory memory{read_image, &image};
    sb::native_render::J3dMaterialState state{};
    GuestColorBlock block{};
    assert(read_guest_color_block(memory, BLOCK, VTABLES, state, block) == GuestColorError::None);

    assert(block.kind == GuestColorBlockKind::LightOn);
    assert(block.blockType == static_cast<std::uint32_t>('CLON'));
    assert(block.lightingEnabled);

    assert(state.supportedColorBlock);
    assert(state.usesMaterialAmbient);
    assert(state.cullMode == 3);
    assert(state.colorChannelCount == 2);
    assert(state.colorChannelControl == (0x0940 | LIGHTING));
    assert(state.alphaChannelControl == 0x0a51);
    assert(state.colorChannelControl1 == 0x0b52);
    assert(state.alphaChannelControl1 == 0x0c53);
    assert(state.lightingEnabled);
    assert(state.materialColorRgba8 == 0x11223344U);
    assert(state.materialColor1Rgba8 == 0x55667788U);
    assert(state.ambientColorRgba8 == 0x99aabbccU);
    assert(state.ambientColor1Rgba8 == 0xddee0f10U);
}

// The two layouts overlap everywhere after the material colours: the unlit block's channel count
// sits where the lit block's first ambient colour starts, and its cull mode sits inside the lit
// block's channel controls. Reading each with its own layout has to answer differently.
void tells_the_two_layouts_apart() {
    Image unlit = build_light_off(2, 0x0940);
    Image lit = build_light_on(2, 0x0940);
    GuestMemory unlitMemory{read_image, &unlit};
    GuestMemory litMemory{read_image, &lit};
    sb::native_render::J3dMaterialState unlitState{};
    sb::native_render::J3dMaterialState litState{};
    GuestColorBlock block{};
    assert(read_guest_color_block(unlitMemory, BLOCK, VTABLES, unlitState, block) ==
           GuestColorError::None);
    assert(read_guest_color_block(litMemory, BLOCK, VTABLES, litState, block) ==
           GuestColorError::None);
    assert(unlitState.cullMode != litState.cullMode);
    assert(unlitState.usesMaterialAmbient != litState.usesMaterialAmbient);
    assert(unlitState.ambientColorRgba8 != litState.ambientColorRgba8);
}

// A block with no colour channels still has material colours and a cull mode; what it has no
// meaningful values for is the channel controls, and the decomp adapter leaves those at zero
// rather than reading them.
void accepts_a_block_with_no_channels() {
    Image image = build_light_on(0, 0x0940);
    GuestMemory memory{read_image, &image};
    sb::native_render::J3dMaterialState state{};
    GuestColorBlock block{};
    assert(read_guest_color_block(memory, BLOCK, VTABLES, state, block) == GuestColorError::None);
    assert(state.colorChannelCount == 0);
    assert(state.colorChannelControl == 0);
    assert(state.alphaChannelControl == 0);
    assert(!state.lightingEnabled);
    assert(state.materialColorRgba8 == 0x11223344U);
    assert(state.cullMode == 3);
}

// One channel means the second colour is not authored, so reading it would answer whatever the
// array holds rather than what the material uses.
void leaves_the_second_channel_unread_when_there_is_one() {
    Image image = build_light_on(1, 0x0940);
    GuestMemory memory{read_image, &image};
    sb::native_render::J3dMaterialState state{};
    GuestColorBlock block{};
    assert(read_guest_color_block(memory, BLOCK, VTABLES, state, block) == GuestColorError::None);
    assert(state.colorChannelCount == 1);
    assert(state.colorChannelControl == 0x0940);
    assert(state.alphaChannelControl == 0x0a51);
    assert(state.colorChannelControl1 == 0);
    assert(state.alphaChannelControl1 == 0);
    assert(state.materialColor1Rgba8 == 0);
    assert(state.ambientColor1Rgba8 == 0);
}

void leaves_the_other_blocks_alone() {
    Image image = build_light_on(2, 0x0940);
    GuestMemory memory{read_image, &image};
    sb::native_render::J3dMaterialState state{};
    state.tevStageCount = 7;
    state.pixelEngineBlockType = static_cast<std::uint32_t>('PEFL');
    state.blendMode = 1;
    GuestColorBlock block{};
    assert(read_guest_color_block(memory, BLOCK, VTABLES, state, block) == GuestColorError::None);
    assert(state.tevStageCount == 7);
    assert(state.pixelEngineBlockType == static_cast<std::uint32_t>('PEFL'));
    assert(state.blendMode == 1);
}

void refuses_what_it_cannot_read() {
    Image image = build_light_on(2, 0x0940);
    sb::native_render::J3dMaterialState state{};
    GuestColorBlock block{};

    const GuestMemory none{nullptr, &image};
    assert(read_guest_color_block(none, BLOCK, VTABLES, state, block) == GuestColorError::NoReader);

    GuestMemory memory{read_image, &image};
    assert(read_guest_color_block(memory, 0, VTABLES, state, block) == GuestColorError::NullBlock);
    assert(read_guest_color_block(memory, 0x70000000, VTABLES, state, block) ==
           GuestColorError::UnreadableBlock);

    {
        Image unknown = build_light_on(2, 0x0940);
        unknown.word(BLOCK + 0x00, 0x80000999);
        GuestMemory unknownMemory{read_image, &unknown};
        assert(read_guest_color_block(unknownMemory, BLOCK, VTABLES, state, block) ==
               GuestColorError::UnknownBlockKind);
    }
    {
        // GX has two colour channels. A third is not a material with more colours; it is a read
        // that landed somewhere else, and answering it would index past the block's own array.
        Image tooMany = build_light_on(3, 0x0940);
        GuestMemory tooManyMemory{read_image, &tooMany};
        assert(read_guest_color_block(tooManyMemory, BLOCK, VTABLES, state, block) ==
               GuestColorError::ColorChannelCountOutOfRange);
    }
    {
        // A block whose vtable pointer is readable but whose body is past the end of memory. The
        // image refuses anything that would run off the end, so the cull mode at 0x40 is the first
        // field that cannot be read.
        Image truncated = build_light_on(0, 0x0940);
        const GuestAddress edge =
            sb::title_adapter::test::RAM_BASE + sb::title_adapter::test::RAM_BYTES - 0x20;
        truncated.word(edge, LIGHT_ON_VTABLE);
        GuestMemory truncatedMemory{read_image, &truncated};
        assert(read_guest_color_block(truncatedMemory, edge, VTABLES, state, block) ==
               GuestColorError::UnreadableCullMode);
    }

    assert(state == sb::native_render::J3dMaterialState{});
}

void agrees_with_the_decomp_block_codes() {
    using sb::title_adapter::guest_color_block_type;
    assert(guest_color_block_type(GuestColorBlockKind::LightOff) ==
           static_cast<std::uint32_t>('CLOF'));
    assert(guest_color_block_type(GuestColorBlockKind::LightOn) ==
           static_cast<std::uint32_t>('CLON'));
}

void names_every_error() {
    for (std::uint8_t value = 0;
         value <= static_cast<std::uint8_t>(GuestColorError::UnreadableCullMode); ++value) {
        const std::string_view text = name(static_cast<GuestColorError>(value));
        assert(!text.empty());
        assert(text != "unknown");
    }
}

} // namespace

int main() {
    reads_an_unlit_block();
    reads_a_lit_block();
    tells_the_two_layouts_apart();
    accepts_a_block_with_no_channels();
    leaves_the_second_channel_unread_when_there_is_one();
    leaves_the_other_blocks_alone();
    refuses_what_it_cannot_read();
    agrees_with_the_decomp_block_codes();
    names_every_error();
    return 0;
}
