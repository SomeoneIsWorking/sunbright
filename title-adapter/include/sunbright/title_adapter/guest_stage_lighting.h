#pragma once

#include <sunbright/native_render/j3d_stage_lighting.h>

#include <sunbright/title_adapter/guest_memory.h>

#include <cstdint>

namespace sb::title_adapter {

// Reading the stage lighting GMSE01 is about to broadcast, at `TLightCommon::setLight`.
//
// This is the one input the shared material classifiers need that no material carries. Every lit
// family takes a `ModelLightingContext`, and without one 37,482 of the 44,314 material packets this
// scene draws are refused for that reason alone.
//
// The seam is the game's own light owner, not GX. `ModelLightingContext` is documented as taking
// the owner's authored world-space values before any console light object is built, so hooking
// `GXLoadLightObjImm` -- which would be easier, since the values there are already transformed --
// would put a console encoding across a boundary that exists to keep them out.
//
// Everything below was recovered from the shipping image rather than taken from the decomp headers,
// and in one place the two disagree. `getAmbColor` (0x80229cec) scales the ambient alpha by the
// f32 at `this + 0x18`; `getLightColor` (0x80229d78) scales the light alpha by the f32 at
// `this + 0x1c`. The decomp declares only one such field, `mAlphaScale` at 0x1c, and uses it for
// both. This reader follows the image.

// GMSE01 US. `TLightMario::setLight` (0x80229610) is a byte-identical override and reads the same.
constexpr GuestAddress GMSE01_LIGHT_COMMON_SET_LIGHT = 0x80229a30;
constexpr GuestAddress GMSE01_LIGHT_MARIO_SET_LIGHT = 0x80229610;
// Three .sbss pointers, all zero in the image and published at scene load: r13-0x6114, r13-0x6118
// and r13-0x610c respectively.
constexpr GuestAddress GMSE01_LIGHT_ARRAY_POINTER = 0x8040e0ac;
constexpr GuestAddress GMSE01_AMBIENT_ARRAY_POINTER = 0x8040e0a8;
constexpr GuestAddress GMSE01_LIGHT_MANAGER_POINTER = 0x8040e0b4;

// Where the three scene-owned pointers live. Parameters rather than baked constants so the tests
// can place their objects anywhere and still drive this exact function.
struct GuestStageLightingAddresses {
    GuestAddress lightArrayPointer = GMSE01_LIGHT_ARRAY_POINTER;
    GuestAddress ambientArrayPointer = GMSE01_AMBIENT_ARRAY_POINTER;
    GuestAddress lightManagerPointer = GMSE01_LIGHT_MANAGER_POINTER;
};

enum class GuestStageLightingError : std::uint8_t {
    None,
    UnreadableLight,
    UnreadableViewMatrix,
    UnreadableLightArray,
    NullLightArray,
    NullLightEntries,
    LightIndexOutOfRange,
    UnreadableLightEntry,
    UnreadableAmbientArray,
    NullAmbientArray,
    NullAmbientEntries,
    AmbientIndexOutOfRange,
    UnreadableAmbientEntry,
    UnreadableLightManager,
    UnreadableEffectLight,
};

// What the read took, so a refusal or an unexpected distribution is attributable. The two `local`
// flags in particular separate a scene that authored its lights in the group arrays from one that
// overrode them on the light object itself, which are different code paths in the game.
struct GuestStageLighting {
    bool usedLocalPosition = false;
    bool usedLocalColor = false;
    std::uint32_t lightSlot = 0;
    std::uint32_t ambientSlot = 0;
    std::int32_t lightCount = 0;
    std::int32_t ambientCount = 0;
    bool effectEnabled = false;
};

[[nodiscard]] const char* guest_stage_lighting_error_name(GuestStageLightingError error) noexcept;

// `light` is `this` and `graphics` the `JDrama::TGraphics*` on entry to `setLight`; `index` is its
// `int` argument. The light getters take `index * 2` and the ambient getter takes `index` itself,
// which is faithful to the image's `slwi r31, r30, 1` and to the raw `r30` the ambient call
// receives.
//
// `out` is written only on success, so a partly-read scene cannot publish half a light rig.
[[nodiscard]] GuestStageLightingError
read_guest_stage_lighting(const GuestMemory& memory, GuestAddress light, GuestAddress graphics,
                          std::int32_t index, const GuestStageLightingAddresses& addresses,
                          native_render::J3dStageLightingInput& out,
                          GuestStageLighting& info) noexcept;

} // namespace sb::title_adapter
