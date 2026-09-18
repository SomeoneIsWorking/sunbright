#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/title_adapter/guest_memory.h>

#include <cstdint>

namespace sb::title_adapter {

// Reads the pixel-engine half of a GMSE01 material out of guest memory.
//
// A J3D material is five polymorphic blocks -- colour, texture generation, TEV, indirect and pixel
// engine -- and this owns the last of them: what the rasteriser does with a shaded fragment.
// `native_render::J3dMaterialState` is the shared normalized form both runtimes fill, so the
// classifiers that turn a material into a `ModelMaterial` are written once and read guest and
// decomp materials alike. This fills that struct's pixel-engine fields; the other blocks fill
// theirs.
//
// The block is one of four classes, and which one it is *is* the raster policy. Three of them --
// J3DPEBlockOpa, J3DPEBlockTexEdge and J3DPEBlockXlu -- carry no state at all: they are a single
// vtable pointer, and their `load()` issues one of three fixed GX configurations. Only
// J3DPEBlockFull holds authored alpha-compare, blend, depth and fog values. So the first thing to
// read is the vtable pointer, and the object's own identity answers most of the question.

enum class GuestPixelEngineKind : std::uint8_t {
    Opaque,      // 'PEOP', J3DPEBlockOpa
    TextureEdge, // 'PEED', J3DPEBlockTexEdge
    Translucent, // 'PEXL', J3DPEBlockXlu
    Full,        // 'PEFL', J3DPEBlockFull
};

[[nodiscard]] std::uint32_t guest_pixel_engine_block_type(GuestPixelEngineKind kind) noexcept;

// GMSE01's four J3DPEBlock vtables, read out of the retail image rather than assumed. Each was
// derived twice: J3DMaterial::createPEBlock (0x802d77a8) stores one of them into each block it
// allocates, and each class's `load` override (J3DPEBlockOpa 0x802d91a4, TexEdge 0x802d931c,
// Xlu 0x802d9490, Full 0x802d9608) appears in the image exactly once, 0x60 past the base the
// allocator stored. The allocation sizes agree independently: four bytes for the three stateless
// blocks, 0x14 for J3DPEBlockFull's fog pointer, alpha compare, blend, depth mode and flags.
constexpr GuestAddress GMSE01_J3D_PE_BLOCK_OPA_VTABLE = 0x803e0e64;
constexpr GuestAddress GMSE01_J3D_PE_BLOCK_TEX_EDGE_VTABLE = 0x803e0e00;
constexpr GuestAddress GMSE01_J3D_PE_BLOCK_XLU_VTABLE = 0x803e0d9c;
constexpr GuestAddress GMSE01_J3D_PE_BLOCK_FULL_VTABLE = 0x803e0968;

struct GuestPixelEngineVtables {
    GuestAddress opaque = GMSE01_J3D_PE_BLOCK_OPA_VTABLE;
    GuestAddress textureEdge = GMSE01_J3D_PE_BLOCK_TEX_EDGE_VTABLE;
    GuestAddress translucent = GMSE01_J3D_PE_BLOCK_XLU_VTABLE;
    GuestAddress full = GMSE01_J3D_PE_BLOCK_FULL_VTABLE;
};

// J3D does not store an alpha-compare or depth-mode configuration; it stores a packed id and looks
// the three fields up in a table built once at boot (makeAlphaCmpTable 0x802eef70,
// makeZModeTable 0x802ef318). Both tables live in .bss, so the retail image carries only zeroes
// and the values have to be read from the running game. Reproducing the encoding here instead
// would be a second implementation of a mapping the game already owns, and one that could not be
// told from the real thing when it disagreed.
constexpr GuestAddress GMSE01_J3D_ALPHA_COMPARE_TABLE = 0x80407150;
constexpr GuestAddress GMSE01_J3D_DEPTH_MODE_TABLE = 0x80407450;

// Entries, not bytes: each holds three u8. `u8 j3dAlphaCmpTable[768]` and `u8 j3dZModeTable[96]`.
constexpr std::uint32_t kGuestAlphaCompareTableEntries = 256;
constexpr std::uint32_t kGuestDepthModeTableEntries = 32;

struct GuestPixelEngineTables {
    GuestAddress alphaCompare = GMSE01_J3D_ALPHA_COMPARE_TABLE;
    GuestAddress depthMode = GMSE01_J3D_DEPTH_MODE_TABLE;
};

enum class GuestPixelEngineError : std::uint8_t {
    None,
    NoReader,
    NullBlock,
    UnreadableBlock,
    UnknownBlockKind,
    UnreadableFog,
    UnreadableAlphaComp,
    UnreadableBlend,
    UnreadableZMode,
    AlphaCompareIdOutOfRange,
    DepthModeIdOutOfRange,
    UnreadableAlphaCompareTable,
    UnreadableDepthModeTable,
};

[[nodiscard]] const char* name(GuestPixelEngineError error) noexcept;

// What was read, for the caller that wants to count block kinds rather than inspect the state.
struct GuestPixelEngineBlock {
    GuestPixelEngineKind kind = GuestPixelEngineKind::Opaque;
    std::uint32_t blockType = 0;
    bool hasFog = false;
    bool hasExplicitPixelPolicy = false;

    // The packed ids as they sit in the guest object, before the lookup. They are carried out
    // because the encoding that produced them is known, so a caller can re-encode what the table
    // answered and compare -- which is the only check here that the table was read at all.
    std::uint16_t alphaCompareId = 0xffff;
    std::uint16_t depthModeId = 0xffff;
};

// Fills `state`'s pixel-engine fields -- block type, alpha compare, blend, depth and fog -- from
// the guest J3DPEBlock at `block`. Every other field of `state` is left alone, so the blocks can
// be read in any order into one material state. Answers an error and leaves `state` untouched
// when the guest object cannot be read or does not describe a block this understands.
[[nodiscard]] GuestPixelEngineError read_guest_pixel_engine_block(
    const GuestMemory& memory, GuestAddress block, const GuestPixelEngineVtables& vtables,
    const GuestPixelEngineTables& tables, native_render::J3dMaterialState& state,
    GuestPixelEngineBlock& out) noexcept;

} // namespace sb::title_adapter
