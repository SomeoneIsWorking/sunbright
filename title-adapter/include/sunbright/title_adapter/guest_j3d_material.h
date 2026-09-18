#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/title_adapter/guest_j3d_color.h>
#include <sunbright/title_adapter/guest_j3d_pixel_engine.h>
#include <sunbright/title_adapter/guest_j3d_tev.h>
#include <sunbright/title_adapter/guest_j3d_texgen.h>
#include <sunbright/title_adapter/guest_memory.h>

#include <cstdint>

namespace sb::title_adapter {

// Reads a whole GMSE01 material into the shared `native_render::J3dMaterialState`.
//
// The material itself is four pointers; every layout decision belongs to the block behind one of
// them, which is why this composes the four block readers rather than reading fields. What it owns
// is the order and the all-or-nothing contract: `state` is written only when all four blocks were
// read, so a caller never classifies a material half of whose program it failed to see.
//
// This is the guest-layout counterpart of `capture_native_j3d_material_state`, which fills the same
// struct from the decomp object layout. There is deliberately no second classifier: everything
// downstream -- the fifteen material families, the fog contract, the raster policy -- reads
// `J3dMaterialState` and cannot tell which runtime filled it.

struct GuestMaterialVtables {
    GuestColorBlockVtables color{};
    GuestTexGenBlockVtables texGen{};
    GuestTevBlockVtables tev{};
    GuestPixelEngineVtables pixelEngine{};
};

enum class GuestMaterialError : std::uint8_t {
    None,
    NoReader,
    NullMaterial,
    UnreadableMaterial,
    ColorBlock,
    TexGenBlock,
    TevBlock,
    PixelEngineBlock,
};

[[nodiscard]] const char* name(GuestMaterialError error) noexcept;

// Which block failed, and why. The per-block error is kept rather than collapsed, because "this
// material could not be read" and "this material's TEV block has no register colours" call for
// different things from the caller.
struct GuestMaterial {
    GuestAddress colorBlock = 0;
    GuestAddress texGenBlock = 0;
    GuestAddress tevBlock = 0;
    GuestAddress pixelEngineBlock = 0;
    GuestColorBlock color{};
    GuestTexGenBlock texGen{};
    GuestTevBlock tev{};
    GuestPixelEngineBlock pixelEngine{};
    GuestColorError colorError = GuestColorError::None;
    GuestTexGenError texGenError = GuestTexGenError::None;
    GuestTevError tevError = GuestTevError::None;
    GuestPixelEngineError pixelEngineError = GuestPixelEngineError::None;
};

// `hasVertexColor` and `hasNormal` describe the geometry the material is about to be applied to,
// not the material, so they are the caller's to supply -- exactly as the decomp adapter takes them.
[[nodiscard]] GuestMaterialError
read_guest_material(const GuestMemory& memory, GuestAddress material,
                    const GuestMaterialVtables& vtables, const GuestPixelEngineTables& tables,
                    bool hasVertexColor, bool hasNormal, native_render::J3dMaterialState& state,
                    GuestMaterial& out) noexcept;

} // namespace sb::title_adapter
