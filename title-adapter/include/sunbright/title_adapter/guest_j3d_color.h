#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/title_adapter/guest_memory.h>

#include <cstdint>

namespace sb::title_adapter {

// Reads the colour half of a GMSE01 material out of guest memory: cull mode, material and ambient
// colours, and the channel controls that decide whether the hardware lights a vertex at all.
//
// Like the pixel-engine block, which class the block is carries meaning rather than merely
// selecting a layout. J3DColorBlockLightOff has no ambient colours and no light table because a
// material that is not lit has nothing to put in them, so 'CLOF' is not 'CLON' with the lighting
// fields left at zero -- the fields are not there. `J3dMaterialState::usesMaterialAmbient` is what
// the shared classifiers read that distinction as.

enum class GuestColorBlockKind : std::uint8_t {
    LightOff, // 'CLOF', J3DColorBlockLightOff
    LightOn,  // 'CLON', J3DColorBlockLightOn
};

[[nodiscard]] std::uint32_t guest_color_block_type(GuestColorBlockKind kind) noexcept;

// GMSE01's two J3DColorBlock vtables, derived twice from the retail image: each class's
// `countDLSize` override (LightOff 0x802d7994, LightOn 0x802d799c) appears exactly once, 0x0c past
// the base, and each class's `load` override (0x802d7aa8, 0x802d7ba0) exactly once, 0x60 past the
// same base. Both offsets match J3DPEBlockFull's, whose base is independently fixed by the
// allocator that stores it.
constexpr GuestAddress GMSE01_J3D_COLOR_BLOCK_LIGHT_OFF_VTABLE = 0x803e0d38;
constexpr GuestAddress GMSE01_J3D_COLOR_BLOCK_LIGHT_ON_VTABLE = 0x803e0cd4;

struct GuestColorBlockVtables {
    GuestAddress lightOff = GMSE01_J3D_COLOR_BLOCK_LIGHT_OFF_VTABLE;
    GuestAddress lightOn = GMSE01_J3D_COLOR_BLOCK_LIGHT_ON_VTABLE;
};

// GX rasterises two colour channels, each with a colour and an alpha control, which is what the
// block's four J3DColorChan entries are. A count past that is a misread rather than a material.
constexpr std::uint8_t kGuestMaxColorChannels = 2;

enum class GuestColorError : std::uint8_t {
    None,
    NoReader,
    NullBlock,
    UnreadableBlock,
    UnknownBlockKind,
    UnreadableChannelCount,
    ColorChannelCountOutOfRange,
    UnreadableChannelControl,
    UnreadableMaterialColor,
    UnreadableAmbientColor,
    UnreadableCullMode,
};

[[nodiscard]] const char* name(GuestColorError error) noexcept;

struct GuestColorBlock {
    GuestColorBlockKind kind = GuestColorBlockKind::LightOff;
    std::uint32_t blockType = 0;
    std::uint8_t colorChannelCount = 0;
    bool lightingEnabled = false;
};

// Fills `state`'s colour fields from the guest J3DColorBlock at `block`, leaving every other field
// alone so the blocks of one material can be read in any order. Answers an error and leaves
// `state` untouched when the guest object cannot be read or does not describe a block this knows.
[[nodiscard]] GuestColorError read_guest_color_block(const GuestMemory& memory, GuestAddress block,
                                                     const GuestColorBlockVtables& vtables,
                                                     native_render::J3dMaterialState& state,
                                                     GuestColorBlock& out) noexcept;

} // namespace sb::title_adapter
