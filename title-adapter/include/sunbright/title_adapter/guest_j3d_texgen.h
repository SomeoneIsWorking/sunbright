#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/title_adapter/guest_memory.h>

#include <cstdint>

namespace sb::title_adapter {

// Reads how many texture coordinates a GMSE01 material generates.
//
// J3D has one texture-generation block class, J3DTexGenBlockBasic ('TGBC'), and the shared
// classifiers use only its coordinate count -- the coordinate definitions and texture matrices
// belong to a later part of the renderer than the material family a state is classified into.
// An unrecognised block is not an error here: the decomp adapter answers a count of
// `0xffffffff` for one, which is the value the classifiers already read as "this material's
// coordinate generation is not something the native path supports". This answers the same, and
// reports separately that it did, so an unrecognised block is counted rather than merely absorbed.

// The vtable of GMSE01's only J3DTexGenBlock class, derived twice from the retail image. Its
// `countDLSize` override (0x802d79a4) appears exactly once, 0x10 past the base, and its `load`
// override (0x802d7cfc) exactly once, 0x48 past it -- the two offsets J3DTexGenBlock's own virtual
// list puts them at, which differs from the colour and pixel-engine blocks because `calc` precedes
// `countDLSize` here.
constexpr GuestAddress GMSE01_J3D_TEX_GEN_BLOCK_BASIC_VTABLE = 0x803e0c84;

// What J3DTexGenBlock::getTexGenNum answers for a block whose type the adapter does not know, as
// the decomp adapter spells it.
constexpr std::uint32_t kGuestUnsupportedTexGenCount = 0xffffffffU;

// GX generates at most eight texture coordinates, which is the size of the block's own array.
constexpr std::uint32_t kGuestMaxTexGenCount = 8;

struct GuestTexGenBlockVtables {
    GuestAddress basic = GMSE01_J3D_TEX_GEN_BLOCK_BASIC_VTABLE;
};

enum class GuestTexGenError : std::uint8_t {
    None,
    NoReader,
    NullBlock,
    UnreadableBlock,
    UnreadableTexGenCount,
    TexGenCountOutOfRange,
};

[[nodiscard]] const char* name(GuestTexGenError error) noexcept;

struct GuestTexGenBlock {
    bool recognised = false;
    std::uint32_t blockType = 0;
    std::uint32_t texGenCount = kGuestUnsupportedTexGenCount;
};

[[nodiscard]] GuestTexGenError read_guest_tex_gen_block(const GuestMemory& memory,
                                                        GuestAddress block,
                                                        const GuestTexGenBlockVtables& vtables,
                                                        native_render::J3dMaterialState& state,
                                                        GuestTexGenBlock& out) noexcept;

} // namespace sb::title_adapter
