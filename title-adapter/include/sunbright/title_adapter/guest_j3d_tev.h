#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/title_adapter/guest_memory.h>

#include <cstdint>

namespace sb::title_adapter {

// Reads the colour-stage half of a GMSE01 material out of guest memory: which textures it binds,
// what each TEV stage computes, and the register and constant colours those stages read.
//
// J3D sizes this block to the material rather than allocating for the worst case, so there are four
// classes with four different layouts and no shared field beyond the texture numbers at 0x04.
// Which class it is bounds the material: a 'TVB2' block has two texture bindings and two stages,
// and no storage at all for a third. That bound is the reason the layouts are stated per class here
// instead of being read from a count -- a stage index past the class's own array is a misread, not
// a material with more stages.
//
// J3DTevBlock1 is the exception that has to be preserved rather than smoothed over: it carries no
// TEV register colours and no constant colours at all, and the decomp adapter's capture fails on
// the first null it gets back for one. A material drawn through a 'TVB1' block therefore does not
// reach the native classifiers from either runtime, and this answers the same rather than
// inventing the colours the block does not have.

enum class GuestTevBlockKind : std::uint8_t {
    Stage1,  // 'TVB1', J3DTevBlock1
    Stage2,  // 'TVB2', J3DTevBlock2
    Stage4,  // 'TVB4', J3DTevBlock4
    Stage16, // 'TV16', J3DTevBlock16
};

[[nodiscard]] std::uint32_t guest_tev_block_type(GuestTevBlockKind kind) noexcept;

// GMSE01's four J3DTevBlock vtables, derived twice from the retail image: each class's
// `countDLSize` override (0x802d79ac, 0x802d79b4, 0x802d79bc, 0x802d79c4) appears exactly once,
// 0x10 past the base, and each class's `load` override (0x802d7e54, 0x802d8040, 0x802d8544,
// 0x802d8a7c) exactly once, 0x98 past the same base. Both offsets are where J3DTevBlock's own
// virtual list puts them, and the two derivations agree for all four.
constexpr GuestAddress GMSE01_J3D_TEV_BLOCK_1_VTABLE = 0x803e0be8;
constexpr GuestAddress GMSE01_J3D_TEV_BLOCK_2_VTABLE = 0x803e0b4c;
constexpr GuestAddress GMSE01_J3D_TEV_BLOCK_4_VTABLE = 0x803e0ab0;
constexpr GuestAddress GMSE01_J3D_TEV_BLOCK_16_VTABLE = 0x803e0a14;

struct GuestTevBlockVtables {
    GuestAddress stage1 = GMSE01_J3D_TEV_BLOCK_1_VTABLE;
    GuestAddress stage2 = GMSE01_J3D_TEV_BLOCK_2_VTABLE;
    GuestAddress stage4 = GMSE01_J3D_TEV_BLOCK_4_VTABLE;
    GuestAddress stage16 = GMSE01_J3D_TEV_BLOCK_16_VTABLE;
};

enum class GuestTevError : std::uint8_t {
    None,
    NoReader,
    NullBlock,
    UnreadableBlock,
    UnknownBlockKind,
    UnreadableStageCount,
    StageCountPastBlockCapacity,
    UnreadableTextureNumber,
    UnreadableTevOrder,
    UnreadableTevStage,
    BlockHasNoTevColors,
    UnreadableTevColor,
    UnreadableKonstColor,
    UnreadableKonstSelection,
};

[[nodiscard]] const char* name(GuestTevError error) noexcept;

struct GuestTevBlock {
    GuestTevBlockKind kind = GuestTevBlockKind::Stage1;
    std::uint32_t blockType = 0;
    std::uint8_t stageCount = 0;
    std::uint8_t stageCapacity = 0;
    std::uint8_t textureBindingCount = 0;
};

// Fills `state`'s colour-stage fields from the guest J3DTevBlock at `block`, leaving every other
// field alone. Answers an error and leaves `state` untouched when the guest object cannot be read
// or does not describe a block this knows.
[[nodiscard]] GuestTevError read_guest_tev_block(const GuestMemory& memory, GuestAddress block,
                                                 const GuestTevBlockVtables& vtables,
                                                 native_render::J3dMaterialState& state,
                                                 GuestTevBlock& out) noexcept;

} // namespace sb::title_adapter
