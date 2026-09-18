// Drives the shipping whole-material reader against a synthetic guest image.
//
// The four block readers have their own tests; what is left to prove here is the composition -- the
// material's four pointers reach the right readers, the four partial fills add up to one state
// rather than overwriting each other, and a material whose fourth block cannot be read does not
// leave three quarters of a new material in the caller's hands.

#include <sunbright/title_adapter/guest_j3d_material.h>

#include "guest_image.h"

#include <array>
#include <cassert>
#include <string_view>

namespace {

using sb::title_adapter::GuestAddress;
using sb::title_adapter::GuestMaterial;
using sb::title_adapter::GuestMaterialError;
using sb::title_adapter::GuestMaterialVtables;
using sb::title_adapter::GuestMemory;
using sb::title_adapter::GuestPixelEngineTables;
using sb::title_adapter::test::Image;
using sb::title_adapter::test::read_image;

constexpr GuestAddress COLOR_VTABLE = 0x80000100;
constexpr GuestAddress TEX_GEN_VTABLE = 0x80000110;
constexpr GuestAddress TEV_VTABLE = 0x80000120;
constexpr GuestAddress PE_VTABLE = 0x80000130;

constexpr GuestAddress MATERIAL = 0x80000200;
constexpr GuestAddress COLOR_BLOCK = 0x80000280;
// The texture-generation block is 0x48 bytes of coordinates and matrix pointers, so it is
// placed clear of the other blocks rather than beside them.
constexpr GuestAddress TEX_GEN_BLOCK = 0x80000a00;
constexpr GuestAddress TEX_MATRIX = 0x80000b00;
constexpr GuestAddress TEV_BLOCK = 0x80000340;
constexpr GuestAddress PE_BLOCK = 0x80000400;
constexpr GuestAddress FOG = 0x80000440;
constexpr GuestAddress ALPHA_TABLE = 0x80000500;
constexpr GuestAddress DEPTH_TABLE = 0x80000900;

constexpr std::uint16_t ALPHA_ID = 0x0021;
constexpr std::uint16_t DEPTH_ID = 0x0005;

const GuestMaterialVtables VTABLES{.color = {.lightOff = 0x80000999, .lightOn = COLOR_VTABLE},
                                   .texGen = {.basic = TEX_GEN_VTABLE},
                                   .tev = {.stage1 = 0x8000099a,
                                           .stage2 = TEV_VTABLE,
                                           .stage4 = 0x8000099b,
                                           .stage16 = 0x8000099c},
                                   .pixelEngine = {.opaque = 0x8000099d,
                                                   .textureEdge = 0x8000099e,
                                                   .translucent = 0x8000099f,
                                                   .full = PE_VTABLE}};
constexpr GuestPixelEngineTables TABLES{.alphaCompare = ALPHA_TABLE, .depthMode = DEPTH_TABLE};

Image build_material() {
    Image image;
    image.word(MATERIAL + 0x20, COLOR_BLOCK);
    image.word(MATERIAL + 0x24, TEX_GEN_BLOCK);
    image.word(MATERIAL + 0x28, TEV_BLOCK);
    image.word(MATERIAL + 0x30, PE_BLOCK);

    // J3DColorBlockLightOn.
    image.word(COLOR_BLOCK + 0x00, COLOR_VTABLE);
    image.byte(COLOR_BLOCK + 0x04, 0x11);
    image.byte(COLOR_BLOCK + 0x05, 0x22);
    image.byte(COLOR_BLOCK + 0x06, 0x33);
    image.byte(COLOR_BLOCK + 0x07, 0x44);
    image.byte(COLOR_BLOCK + 0x0c, 0x99);
    image.byte(COLOR_BLOCK + 0x0d, 0xaa);
    image.byte(COLOR_BLOCK + 0x0e, 0xbb);
    image.byte(COLOR_BLOCK + 0x0f, 0xcc);
    image.byte(COLOR_BLOCK + 0x14, 1);      // one colour channel
    image.half(COLOR_BLOCK + 0x16, 0x0002); // lit
    image.half(COLOR_BLOCK + 0x18, 0x0a51);
    image.byte(COLOR_BLOCK + 0x40, 2); // cull mode

    // J3DTexGenBlockBasic: one coordinate, a 2x4 multiply of authored set 0 by texture matrix 0.
    image.word(TEX_GEN_BLOCK + 0x00, TEX_GEN_VTABLE);
    image.word(TEX_GEN_BLOCK + 0x04, 1);
    image.byte(TEX_GEN_BLOCK + 0x08, 1);  // MTX2x4
    image.byte(TEX_GEN_BLOCK + 0x09, 4);  // authored coordinate set 0
    image.byte(TEX_GEN_BLOCK + 0x0a, 30); // the first loadable texture matrix
    image.word(TEX_GEN_BLOCK + 0x28, TEX_MATRIX);
    image.byte(TEX_MATRIX + 0x00, 1); // projection
    image.byte(TEX_MATRIX + 0x01, 0); // info
    image.real(TEX_MATRIX + 0x64, 2.0F);
    image.real(TEX_MATRIX + 0x78, 3.0F);
    image.real(TEX_MATRIX + 0x8c, 1.0F);

    // J3DTevBlock2 with one active stage.
    image.word(TEV_BLOCK + 0x00, TEV_VTABLE);
    image.half(TEV_BLOCK + 0x04, 0x0007);
    image.half(TEV_BLOCK + 0x06, 0x0008);
    image.byte(TEV_BLOCK + 0x08, 0); // order 0: coordinate
    image.byte(TEV_BLOCK + 0x09, 0); // map
    image.byte(TEV_BLOCK + 0x0a, 4); // colour channel
    image.byte(TEV_BLOCK + 0x30, 1); // one stage
    for (std::uint8_t byte = 0; byte < 8; ++byte) {
        image.byte(TEV_BLOCK + 0x31 + byte, static_cast<std::uint8_t>(0x50 + byte));
    }

    // J3DPEBlockFull.
    image.word(PE_BLOCK + 0x00, PE_VTABLE);
    image.word(PE_BLOCK + 0x04, FOG);
    image.half(PE_BLOCK + 0x08, ALPHA_ID);
    image.byte(PE_BLOCK + 0x0a, 0x80);
    image.byte(PE_BLOCK + 0x0c, 1); // blend mode
    image.byte(PE_BLOCK + 0x0d, 4);
    image.byte(PE_BLOCK + 0x0e, 5);
    image.half(PE_BLOCK + 0x10, DEPTH_ID);
    image.byte(FOG + 0x00, 2);
    image.real(FOG + 0x08, 512.0F);

    image.byte(ALPHA_TABLE + ALPHA_ID * 3 + 0, 4);
    image.byte(ALPHA_TABLE + ALPHA_ID * 3 + 1, 0);
    image.byte(ALPHA_TABLE + ALPHA_ID * 3 + 2, 7);
    image.byte(DEPTH_TABLE + DEPTH_ID * 3 + 0, 1);
    image.byte(DEPTH_TABLE + DEPTH_ID * 3 + 1, 3);
    image.byte(DEPTH_TABLE + DEPTH_ID * 3 + 2, 1);
    return image;
}

// Every block contributes a field nothing else writes, so one state holding all four is the whole
// composition being exercised rather than the last reader's fill.
void reads_all_four_blocks_into_one_state() {
    Image image = build_material();
    GuestMemory memory{read_image, &image};
    sb::native_render::J3dMaterialState state{};
    GuestMaterial material{};
    assert(read_guest_material(memory, MATERIAL, VTABLES, TABLES, true, true, state, material) ==
           GuestMaterialError::None);

    assert(material.colorBlock == COLOR_BLOCK);
    assert(material.texGenBlock == TEX_GEN_BLOCK);
    assert(material.texGen.texGenCount == 1);
    assert(material.texGen.coordinates[0] ==
           (sb::title_adapter::GuestTexCoordDefinition{
               .texGenType = 1, .texGenSrc = 4, .texGenMatrix = 30}));
    assert(material.texGen.matrices[0].present);
    assert(material.texGen.matrices[0].projection == 1);
    assert(material.texGen.matrices[0].total[0] == 2.0F);
    assert(material.texGen.matrices[0].total[5] == 3.0F);
    assert(material.texGen.matrices[0].total[10] == 1.0F);
    assert(!material.texGen.matrices[1].present);
    assert(material.tevBlock == TEV_BLOCK);
    assert(material.pixelEngineBlock == PE_BLOCK);

    // Colour block.
    assert(state.supportedColorBlock);
    assert(state.usesMaterialAmbient);
    assert(state.lightingEnabled);
    assert(state.cullMode == 2);
    assert(state.materialColorRgba8 == 0x11223344U);
    assert(state.ambientColorRgba8 == 0x99aabbccU);
    // Texture generation block.
    assert(state.textureCoordinateCount == 1);
    // TEV block.
    assert(state.tevBlockType == static_cast<std::uint32_t>('TVB2'));
    assert(state.tevStageCount == 1);
    assert(state.textureBindings[0].textureNumber == 7);
    assert(state.tevStages[0].colorChannel == 4);
    assert(state.tevStages[0].program[0] == 0x50);
    // Pixel engine block.
    assert(state.pixelEngineBlockType == static_cast<std::uint32_t>('PEFL'));
    assert(state.hasExplicitPixelPolicy);
    assert(state.alphaCompare0 == 4);
    assert(state.alphaReference0 == 0x80);
    assert(state.blendSourceFactor == 4);
    assert(state.depthTest);
    assert(state.depthWrite);
    assert(state.fog.type == 2);
    assert(state.fog.end == 512.0F);
    // The caller's description of the geometry, which belongs to neither block.
    assert(state.hasVertexColor);
    assert(state.hasNormal);
}

// The geometry flags are the caller's, so they must survive every block's partial fill.
void carries_the_callers_geometry_flags() {
    Image image = build_material();
    GuestMemory memory{read_image, &image};
    sb::native_render::J3dMaterialState state{};
    GuestMaterial material{};
    assert(read_guest_material(memory, MATERIAL, VTABLES, TABLES, false, false, state, material) ==
           GuestMaterialError::None);
    assert(!state.hasVertexColor);
    assert(!state.hasNormal);
}

// A material is read whole or not at all: three blocks having been read is not a material, and
// publishing them would classify a program the fourth block was going to change.
void publishes_nothing_when_a_block_fails() {
    struct Case {
        GuestAddress pointer;
        GuestMaterialError error;
    };
    const std::array<Case, 4> cases{{
        {MATERIAL + 0x20, GuestMaterialError::ColorBlock},
        {MATERIAL + 0x24, GuestMaterialError::TexGenBlock},
        {MATERIAL + 0x28, GuestMaterialError::TevBlock},
        {MATERIAL + 0x30, GuestMaterialError::PixelEngineBlock},
    }};
    for (const Case& testCase : cases) {
        Image image = build_material();
        image.word(testCase.pointer, 0);
        GuestMemory memory{read_image, &image};
        sb::native_render::J3dMaterialState state{};
        state.cullMode = 0xEE;
        GuestMaterial material{};
        assert(read_guest_material(memory, MATERIAL, VTABLES, TABLES, true, true, state,
                                   material) == testCase.error);
        assert(state.cullMode == 0xEE);
        assert(state == [] {
            sb::native_render::J3dMaterialState untouched{};
            untouched.cullMode = 0xEE;
            return untouched;
        }());
    }
}

void refuses_what_it_cannot_read() {
    Image image = build_material();
    sb::native_render::J3dMaterialState state{};
    GuestMaterial material{};

    const GuestMemory none{nullptr, &image};
    assert(read_guest_material(none, MATERIAL, VTABLES, TABLES, true, true, state, material) ==
           GuestMaterialError::NoReader);

    GuestMemory memory{read_image, &image};
    assert(read_guest_material(memory, 0, VTABLES, TABLES, true, true, state, material) ==
           GuestMaterialError::NullMaterial);
    assert(read_guest_material(memory, 0x70000000, VTABLES, TABLES, true, true, state, material) ==
           GuestMaterialError::UnreadableMaterial);
    assert(state == sb::native_render::J3dMaterialState{});
}

void names_every_error() {
    for (std::uint8_t value = 0;
         value <= static_cast<std::uint8_t>(GuestMaterialError::PixelEngineBlock); ++value) {
        const std::string_view text = name(static_cast<GuestMaterialError>(value));
        assert(!text.empty());
        assert(text != "unknown");
    }
}

} // namespace

int main() {
    reads_all_four_blocks_into_one_state();
    carries_the_callers_geometry_flags();
    publishes_nothing_when_a_block_fails();
    refuses_what_it_cannot_read();
    names_every_error();
    return 0;
}
