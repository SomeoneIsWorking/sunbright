// Drives the shipping pixel-engine reader against a synthetic guest image.
//
// Most of what this reader gets wrong would still produce a valid-looking raster policy: a blend
// factor read one byte late is still a blend factor, and an alpha-compare id resolved through the
// wrong table row is still a comparison. The fixture therefore gives every field a value that
// belongs to no other field, so a misread lands on something the assertions name rather than on a
// neighbour's plausible value.

#include <sunbright/title_adapter/guest_j3d_pixel_engine.h>

#include "guest_image.h"

#include <array>
#include <cassert>
#include <string_view>

namespace {

using sb::title_adapter::GuestAddress;
using sb::title_adapter::GuestMemory;
using sb::title_adapter::GuestPixelEngineBlock;
using sb::title_adapter::GuestPixelEngineError;
using sb::title_adapter::GuestPixelEngineKind;
using sb::title_adapter::GuestPixelEngineTables;
using sb::title_adapter::GuestPixelEngineVtables;
using sb::title_adapter::test::Image;
using sb::title_adapter::test::read_image;

constexpr GuestAddress OPA_VTABLE = 0x80000100;
constexpr GuestAddress TEX_EDGE_VTABLE = 0x80000110;
constexpr GuestAddress XLU_VTABLE = 0x80000120;
constexpr GuestAddress FULL_VTABLE = 0x80000130;

constexpr GuestAddress BLOCK = 0x80000200;
constexpr GuestAddress FOG = 0x80000240;
constexpr GuestAddress ALPHA_COMPARE_TABLE = 0x80000400;
constexpr GuestAddress DEPTH_MODE_TABLE = 0x80000900;

// Ids chosen so neither indexes a row another id would reach, and so neither is zero: a reader
// that ignored the id entirely would read row 0 of both tables and agree with a fixture that used
// them.
constexpr std::uint16_t ALPHA_COMPARE_ID = 0x0057;
constexpr std::uint16_t DEPTH_MODE_ID = 0x0013;

constexpr GuestPixelEngineVtables VTABLES{.opaque = OPA_VTABLE,
                                          .textureEdge = TEX_EDGE_VTABLE,
                                          .translucent = XLU_VTABLE,
                                          .full = FULL_VTABLE};
constexpr GuestPixelEngineTables TABLES{.alphaCompare = ALPHA_COMPARE_TABLE,
                                        .depthMode = DEPTH_MODE_TABLE};

Image build_full_block() {
    Image image;
    image.word(BLOCK + 0x00, FULL_VTABLE);
    image.word(BLOCK + 0x04, FOG);
    image.half(BLOCK + 0x08, ALPHA_COMPARE_ID);
    image.byte(BLOCK + 0x0a, 0x31); // mRef0
    image.byte(BLOCK + 0x0b, 0x72); // mRef1
    image.byte(BLOCK + 0x0c, 0x11); // mBlendMode
    image.byte(BLOCK + 0x0d, 0x22); // mSrcFactor
    image.byte(BLOCK + 0x0e, 0x33); // mDstFactor
    image.byte(BLOCK + 0x0f, 0x44); // mLogicOp
    image.half(BLOCK + 0x10, DEPTH_MODE_ID);
    image.byte(BLOCK + 0x12, 1); // mZCompLoc
    image.byte(BLOCK + 0x13, 1); // mDither

    image.byte(FOG + 0x00, 2);      // mType
    image.byte(FOG + 0x01, 1);      // mAdjEnable
    image.half(FOG + 0x02, 0x0140); // mCenter
    image.real(FOG + 0x04, 128.0F); // mStartZ
    image.real(FOG + 0x08, 4096.0F);
    image.real(FOG + 0x0c, 1.0F);
    image.real(FOG + 0x10, 8192.0F);
    image.byte(FOG + 0x14, 0x10); // mColor
    image.byte(FOG + 0x15, 0x20);
    image.byte(FOG + 0x16, 0x30);
    image.byte(FOG + 0x17, 0x40);
    for (std::uint16_t entry = 0; entry < 10; ++entry) {
        image.half(FOG + 0x18 + entry * 2, static_cast<std::uint16_t>(0x0100 + entry));
    }

    // The two runtime tables, populated only at the rows the ids name.
    image.byte(ALPHA_COMPARE_TABLE + ALPHA_COMPARE_ID * 3 + 0, 6); // comp0
    image.byte(ALPHA_COMPARE_TABLE + ALPHA_COMPARE_ID * 3 + 1, 1); // op
    image.byte(ALPHA_COMPARE_TABLE + ALPHA_COMPARE_ID * 3 + 2, 4); // comp1
    image.byte(DEPTH_MODE_TABLE + DEPTH_MODE_ID * 3 + 0, 1);       // compare enable
    image.byte(DEPTH_MODE_TABLE + DEPTH_MODE_ID * 3 + 1, 3);       // func
    image.byte(DEPTH_MODE_TABLE + DEPTH_MODE_ID * 3 + 2, 0);       // update enable
    return image;
}

void reads_a_full_block() {
    Image image = build_full_block();
    GuestMemory memory{read_image, &image};
    sb::native_render::J3dMaterialState state{};
    GuestPixelEngineBlock block{};
    assert(read_guest_pixel_engine_block(memory, BLOCK, VTABLES, TABLES, state, block) ==
           GuestPixelEngineError::None);

    assert(block.kind == GuestPixelEngineKind::Full);
    assert(block.blockType == static_cast<std::uint32_t>('PEFL'));
    assert(block.hasFog);
    assert(block.hasExplicitPixelPolicy);
    assert(block.alphaCompareId == ALPHA_COMPARE_ID);
    assert(block.depthModeId == DEPTH_MODE_ID);

    assert(state.pixelEngineBlockType == static_cast<std::uint32_t>('PEFL'));
    assert(state.hasExplicitPixelPolicy);
    assert(state.alphaCompare0 == 6);
    assert(state.alphaOperation == 1);
    assert(state.alphaCompare1 == 4);
    assert(state.alphaReference0 == 0x31);
    assert(state.alphaReference1 == 0x72);
    assert(state.blendMode == 0x11);
    assert(state.blendSourceFactor == 0x22);
    assert(state.blendDestinationFactor == 0x33);
    assert(state.blendLogicOperation == 0x44);
    assert(state.depthTest);
    assert(state.depthCompare == 3);
    assert(!state.depthWrite);

    assert(state.fog.type == 2);
    assert(state.fog.rangeAdjustmentEnabled);
    assert(state.fog.center == 0x0140);
    assert(state.fog.start == 128.0F);
    assert(state.fog.end == 4096.0F);
    assert(state.fog.near == 1.0F);
    assert(state.fog.far == 8192.0F);
    assert(state.fog.colorRgba8 == 0x10203040U);
    for (std::uint16_t entry = 0; entry < state.fog.rangeAdjustmentTable.size(); ++entry) {
        assert(state.fog.rangeAdjustmentTable[entry] == 0x0100 + entry);
    }
}

// The three stateless blocks are four bytes of vtable pointer and nothing else. Reading anything
// past that would be reading whatever the allocator put next; the assertions here are as much
// about what is *not* read as what is.
void reads_the_stateless_blocks() {
    struct Case {
        GuestAddress vtable;
        GuestPixelEngineKind kind;
        std::uint32_t type;
    };
    const std::array<Case, 3> cases{{
        {OPA_VTABLE, GuestPixelEngineKind::Opaque, static_cast<std::uint32_t>('PEOP')},
        {TEX_EDGE_VTABLE, GuestPixelEngineKind::TextureEdge, static_cast<std::uint32_t>('PEED')},
        {XLU_VTABLE, GuestPixelEngineKind::Translucent, static_cast<std::uint32_t>('PEXL')},
    }};
    for (const Case& testCase : cases) {
        // The block is deliberately built as a full block first and then re-pointed, so anything
        // the reader touches beyond the vtable pointer would produce a populated policy.
        Image image = build_full_block();
        image.word(BLOCK + 0x00, testCase.vtable);
        GuestMemory memory{read_image, &image};
        sb::native_render::J3dMaterialState state{};
        GuestPixelEngineBlock block{};
        assert(read_guest_pixel_engine_block(memory, BLOCK, VTABLES, TABLES, state, block) ==
               GuestPixelEngineError::None);
        assert(block.kind == testCase.kind);
        assert(block.blockType == testCase.type);
        assert(!block.hasFog);
        assert(!block.hasExplicitPixelPolicy);
        assert(state.pixelEngineBlockType == testCase.type);
        assert(!state.hasExplicitPixelPolicy);
        assert(state.blendMode == 0);
        assert(state.depthCompare == 0);
        assert(state.fog == sb::native_render::J3dFogState{});
    }
}

// A full block may leave either id unauthored, in which case its `load()` issues nothing and the
// rasteriser keeps whatever the previous material set. There is no policy to report, and the
// fields must stay clear rather than carry a row of the table the unauthored id does not name.
void reports_no_policy_for_an_unauthored_id() {
    const std::array<GuestAddress, 2> idFields{BLOCK + 0x08, BLOCK + 0x10};
    for (const GuestAddress field : idFields) {
        Image image = build_full_block();
        image.half(field, 0xffff);
        GuestMemory memory{read_image, &image};
        sb::native_render::J3dMaterialState state{};
        GuestPixelEngineBlock block{};
        assert(read_guest_pixel_engine_block(memory, BLOCK, VTABLES, TABLES, state, block) ==
               GuestPixelEngineError::None);
        assert(block.kind == GuestPixelEngineKind::Full);
        assert(!block.hasExplicitPixelPolicy);
        assert(block.alphaCompareId == 0xffff || block.depthModeId == 0xffff);
        assert(!state.hasExplicitPixelPolicy);
        assert(state.alphaCompare0 == 0);
        assert(state.blendMode == 0);
        assert(state.depthCompare == 0);
        // Fog is authored separately from the raster policy and is still read.
        assert(state.fog.type == 2);
    }
}

// A full block with no fog is ordinary, not an error: J3DPEBlockFull::getFog() answers null and the
// decomp adapter leaves the fog state default.
void accepts_a_full_block_without_fog() {
    Image image = build_full_block();
    image.word(BLOCK + 0x04, 0);
    GuestMemory memory{read_image, &image};
    sb::native_render::J3dMaterialState state{};
    GuestPixelEngineBlock block{};
    assert(read_guest_pixel_engine_block(memory, BLOCK, VTABLES, TABLES, state, block) ==
           GuestPixelEngineError::None);
    assert(!block.hasFog);
    assert(state.fog == sb::native_render::J3dFogState{});
    assert(state.hasExplicitPixelPolicy);
}

// Every other block of a material writes its own fields of the same state, so this one must leave
// them alone rather than start from a blank material.
void leaves_the_other_blocks_alone() {
    Image image = build_full_block();
    GuestMemory memory{read_image, &image};
    sb::native_render::J3dMaterialState state{};
    state.cullMode = 2;
    state.tevStageCount = 5;
    state.materialColorRgba8 = 0xaabbccddU;
    GuestPixelEngineBlock block{};
    assert(read_guest_pixel_engine_block(memory, BLOCK, VTABLES, TABLES, state, block) ==
           GuestPixelEngineError::None);
    assert(state.cullMode == 2);
    assert(state.tevStageCount == 5);
    assert(state.materialColorRgba8 == 0xaabbccddU);
}

void refuses_what_it_cannot_read() {
    Image image = build_full_block();
    sb::native_render::J3dMaterialState state{};
    GuestPixelEngineBlock block{};

    const GuestMemory none{nullptr, &image};
    assert(read_guest_pixel_engine_block(none, BLOCK, VTABLES, TABLES, state, block) ==
           GuestPixelEngineError::NoReader);

    GuestMemory memory{read_image, &image};
    assert(read_guest_pixel_engine_block(memory, 0, VTABLES, TABLES, state, block) ==
           GuestPixelEngineError::NullBlock);
    assert(read_guest_pixel_engine_block(memory, 0x70000000, VTABLES, TABLES, state, block) ==
           GuestPixelEngineError::UnreadableBlock);

    {
        Image unknown = build_full_block();
        unknown.word(BLOCK + 0x00, 0x80000999);
        GuestMemory unknownMemory{read_image, &unknown};
        assert(read_guest_pixel_engine_block(unknownMemory, BLOCK, VTABLES, TABLES, state, block) ==
               GuestPixelEngineError::UnknownBlockKind);
    }
    {
        Image unreadableFog = build_full_block();
        unreadableFog.word(BLOCK + 0x04, 0x70000000);
        GuestMemory fogMemory{read_image, &unreadableFog};
        assert(read_guest_pixel_engine_block(fogMemory, BLOCK, VTABLES, TABLES, state, block) ==
               GuestPixelEngineError::UnreadableFog);
    }
    {
        // An id past the end of the table the game built. Answering with the bytes that happen to
        // follow the table would be a raster policy nothing authored.
        Image wideAlpha = build_full_block();
        wideAlpha.half(BLOCK + 0x08, 0x0100);
        GuestMemory alphaMemory{read_image, &wideAlpha};
        assert(read_guest_pixel_engine_block(alphaMemory, BLOCK, VTABLES, TABLES, state, block) ==
               GuestPixelEngineError::AlphaCompareIdOutOfRange);
    }
    {
        Image wideDepth = build_full_block();
        wideDepth.half(BLOCK + 0x10, 0x0020);
        GuestMemory depthMemory{read_image, &wideDepth};
        assert(read_guest_pixel_engine_block(depthMemory, BLOCK, VTABLES, TABLES, state, block) ==
               GuestPixelEngineError::DepthModeIdOutOfRange);
    }
    {
        Image unmappedTables = build_full_block();
        GuestMemory tableMemory{read_image, &unmappedTables};
        const GuestPixelEngineTables missingAlpha{.alphaCompare = 0x70000000,
                                                  .depthMode = DEPTH_MODE_TABLE};
        assert(read_guest_pixel_engine_block(tableMemory, BLOCK, VTABLES, missingAlpha, state,
                                             block) ==
               GuestPixelEngineError::UnreadableAlphaCompareTable);
        const GuestPixelEngineTables missingDepth{.alphaCompare = ALPHA_COMPARE_TABLE,
                                                  .depthMode = 0x70000000};
        assert(read_guest_pixel_engine_block(tableMemory, BLOCK, VTABLES, missingDepth, state,
                                             block) ==
               GuestPixelEngineError::UnreadableDepthModeTable);
    }

    // Nothing above was allowed to leave a half-read policy behind.
    assert(state == sb::native_render::J3dMaterialState{});
}

// The four type codes cross a runtime boundary: `native_render`'s classifiers compare against the
// decomp adapter's `static_cast<std::uint32_t>('PEFL')`, so a guest reader that spelled one of
// them differently would classify the same material differently depending on which runtime read
// it. Stating both spellings here is what makes that impossible rather than merely unlikely.
void agrees_with_the_decomp_block_codes() {
    using sb::title_adapter::guest_pixel_engine_block_type;
    static_assert(sizeof('PEOP') == sizeof(std::uint32_t));
    assert(guest_pixel_engine_block_type(GuestPixelEngineKind::Opaque) ==
           static_cast<std::uint32_t>('PEOP'));
    assert(guest_pixel_engine_block_type(GuestPixelEngineKind::TextureEdge) ==
           static_cast<std::uint32_t>('PEED'));
    assert(guest_pixel_engine_block_type(GuestPixelEngineKind::Translucent) ==
           static_cast<std::uint32_t>('PEXL'));
    assert(guest_pixel_engine_block_type(GuestPixelEngineKind::Full) ==
           static_cast<std::uint32_t>('PEFL'));
}

void names_every_error() {
    for (std::uint8_t value = 0;
         value <= static_cast<std::uint8_t>(GuestPixelEngineError::UnreadableDepthModeTable);
         ++value) {
        const std::string_view text = name(static_cast<GuestPixelEngineError>(value));
        assert(!text.empty());
        assert(text != "unknown");
    }
}

} // namespace

int main() {
    reads_a_full_block();
    reads_the_stateless_blocks();
    reports_no_policy_for_an_unauthored_id();
    accepts_a_full_block_without_fog();
    leaves_the_other_blocks_alone();
    refuses_what_it_cannot_read();
    agrees_with_the_decomp_block_codes();
    names_every_error();
    return 0;
}
