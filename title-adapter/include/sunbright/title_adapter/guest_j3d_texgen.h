#pragma once

#include <sunbright/native_render/j3d_material_state.h>
#include <sunbright/native_render/j3d_tex_coord_generation.h>
#include <sunbright/title_adapter/guest_memory.h>

#include <array>
#include <cstdint>

namespace sb::title_adapter {

// Reads how a GMSE01 material generates its texture coordinates.
//
// J3D has one texture-generation block class, J3DTexGenBlockBasic ('TGBC'). The shared classifiers
// use only its coordinate count, because the family a material belongs to does not depend on where
// its coordinates come from. Drawing it does: a coordinate names a source and a matrix, and a
// material that scales or scrolls a small texture across a surface does it entirely through that
// matrix. Reading the count alone draws the texture at whatever scale the vertices happen to carry,
// which is how an eight-texel detail texture ends up magnified across a whole screen.
//
// The matrix is read, not recomputed. `J3DTexMtx::calc` runs every frame in the title and leaves
// its result in `mTotalMtx`; taking that is the same number the console's GX would have been given,
// where a second implementation of the SRT composition would be a second source of truth for it.
//
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
    UnreadableTexCoord,
    UnreadableTexMatrixPointer,
    UnreadableTexMatrix,
};

[[nodiscard]] const char* name(GuestTexGenError error) noexcept;

// GX's texture-matrix identifiers, as J3DTexCoord stores them in `mTexGenMtx`. `GX_IDENTITY` means
// the coordinate passes through untransformed; the nine loadable matrices are three apart because
// each occupies three rows of the transform memory.
inline constexpr std::uint8_t kGuestTexGenMatrixIdentity = 60;
inline constexpr std::uint8_t kGuestTexGenMatrix0 = 30;
inline constexpr std::uint8_t kGuestTexGenMatrixStride = 3;
inline constexpr std::uint32_t kGuestMaxTexMatrixCount = 8;

// Which of a block's eight matrix slots a `mTexGenMtx` value names, or `kGuestMaxTexMatrixCount`
// for `GX_IDENTITY` and for any value that is not one of the nine loadable matrices.
[[nodiscard]] std::uint32_t guest_tex_gen_matrix_slot(std::uint8_t texGenMatrix) noexcept;

// One entry of J3DTexGenBlockBasic::mTexCoord: a GX texture-coordinate generator.
struct GuestTexCoordDefinition {
    // GXTexGenType: 0 = MTX3x4, 1 = MTX2x4, 2/3 = BUMP, 4/5 = SRTG.
    std::uint8_t texGenType = 0;
    // GXTexGenSrc: 0 = POS, 1 = NRM, 4..11 = TEX0..TEX7, and the rest colour and binormal sources.
    std::uint8_t texGenSrc = 0;
    std::uint8_t texGenMatrix = kGuestTexGenMatrixIdentity;
    bool operator==(const GuestTexCoordDefinition&) const = default;
};

// One of J3DTexGenBlockBasic::mTexMtx, as the title left it after its own per-frame `calc`.
struct GuestTexMatrix {
    // False when the block's slot holds a null pointer, which is the ordinary case for a material
    // that transforms none of its coordinates. A null slot and a slot whose matrix could not be
    // read are different answers and are kept apart.
    bool present = false;
    // J3DTexMtxInfo::mProjection and mInfo: which projection the matrix was built for and how its
    // effect matrix participates. Carried so a consumer can refuse a combination it does not
    // implement rather than silently applying the matrix as if it were an ordinary 3x4.
    std::uint8_t projection = 0;
    std::uint8_t info = 0;
    // J3DTexMtx::mTotalMtx, row-major, the three rows GX loads.
    std::array<float, 12> total{};
    bool operator==(const GuestTexMatrix&) const = default;
};

struct GuestTexGenBlock {
    bool recognised = false;
    std::uint32_t blockType = 0;
    std::uint32_t texGenCount = kGuestUnsupportedTexGenCount;
    std::array<GuestTexCoordDefinition, kGuestMaxTexGenCount> coordinates{};
    std::array<GuestTexMatrix, kGuestMaxTexMatrixCount> matrices{};
};

// Restates a block's generators as the renderer-neutral description the coordinate step takes.
//
// It is a translation, not a decision: every value comes from the block, and a generator whose
// matrix slot the block left empty is reported as having no matrix rather than as having an
// identity one. An unrecognised block has no generators to restate and answers a count of zero,
// which the coordinate step treats as leaving the authored coordinates alone.
[[nodiscard]] native_render::J3dTexCoordGeneration
build_guest_tex_coord_generation(const GuestTexGenBlock& block) noexcept;

[[nodiscard]] GuestTexGenError read_guest_tex_gen_block(const GuestMemory& memory,
                                                        GuestAddress block,
                                                        const GuestTexGenBlockVtables& vtables,
                                                        native_render::J3dMaterialState& state,
                                                        GuestTexGenBlock& out) noexcept;

} // namespace sb::title_adapter
