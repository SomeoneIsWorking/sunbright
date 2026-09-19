#pragma once

#include <sunbright/native_render/model.h>
#include <sunbright/title_adapter/guest_j3d_shape.h>
#include <sunbright/title_adapter/guest_memory.h>

#include <array>
#include <cstdint>

namespace sb::title_adapter {

// Reads the skeletal pose GMSE01 is about to draw a matrix group with.
//
// `read_guest_shape` answers what a vertex looks like; this answers where it goes. The two are
// separate because they are separate questions about separate guest objects: the vertex
// description belongs to the shape and its model data, while the pose belongs to the model
// instance and changes every frame.
//
// The whole mapping is stated by J3DShape::draw (0x802e0390) and J3DShapeMtx::load, which the
// decomp reproduces exactly:
//
//     J3DShapeMtx::currentPipeline = mFlags >> 2 & 3;
//     j3dSys.setModelDrawMtx(mDrawMatrices[*mCurrentViewNo], mDrawMtxData->mEntryNum);
//     j3dSys.setModelNrmMtx(mNormMatrices[*mCurrentViewNo], mDrawMtxData->mEntryNum);
//     for each matrix group i: mMatrices[i]->load();
//
// and `load()` issues, per matrix it uses, `GXLoadPosMtxIndx(drawMatrixIndex, slot * 3)`. So the
// display list's PNMTXIDX values -- which `native_render::J3dDecodedVertex` carries through as
// `positionMatrixSlot` -- name a slot in *this* group's palette, not a matrix in the model. That
// indirection is the whole reason a pose has to be read per matrix group rather than per shape.
//
// A matrix group does not have to fill every slot it uses. `J3DShapeMtxMulti::load` skips a slot
// whose index is 0xffff, and skipping is not the same as leaving it empty: GX has no unused
// matrix register, so the slot keeps whatever the previous matrix group loaded into it, and the
// authored geometry draws through it. Measured on GMSE01: of the matrix groups that declare ten
// slots, most fill only three to eight, the gaps fall between filled slots rather than after them,
// and their display lists reference the gaps. Reading each group in isolation therefore cannot
// produce its pose -- the state has to be carried, which is what `GuestMatrixRegisters` is for.

// GX has ten position/normal matrix slots. `J3DShapeMtx::load` loads its i'th matrix into slot i,
// by passing `i * 3` -- the matrix *register* index -- to GXLoadPosMtxIndx, and the display list's
// per-vertex selector holds that same register index. `native-render`'s decoder owns the conversion
// back to a slot and refuses a register that is not one, so this table is indexed by the slot the
// decoder publishes rather than by the register, and the two domains cannot drift apart.
constexpr std::uint32_t kGuestMatrixSlotCount = native_render::kJ3dMatrixSlotCount;
constexpr std::uint16_t kGuestUnusedMatrixIndex = 0xffff;

// Which of the four matrix-load pipelines J3DShape::draw selected, from `mFlags >> 2 & 3`. The two
// CPU-position forms matter to a renderer: for those the game has already skinned positions into
// view space on the CPU, so the position matrix is the view matrix itself and the draw matrix
// applies to normals only. Treating them as indexed would transform already-transformed vertices.
enum class GuestSkinningPipeline : std::uint8_t {
    IndexedPositionAndNormal, // PNGP
    CpuPosition,              // PCPU
    CpuNormal,                // NCPU
    CpuPositionAndNormal,     // PNCPU
};

[[nodiscard]] constexpr bool
guest_pipeline_positions_are_view_space(GuestSkinningPipeline pipeline) noexcept {
    return pipeline == GuestSkinningPipeline::CpuPosition ||
           pipeline == GuestSkinningPipeline::CpuPositionAndNormal;
}

[[nodiscard]] constexpr bool
guest_pipeline_normals_are_view_space(GuestSkinningPipeline pipeline) noexcept {
    return pipeline == GuestSkinningPipeline::CpuNormal ||
           pipeline == GuestSkinningPipeline::CpuPositionAndNormal;
}

[[nodiscard]] const char* guest_skinning_pipeline_name(GuestSkinningPipeline pipeline) noexcept;

// Which J3DShapeMtx subclass the matrix group is, identified by the guest vtable pointer the object
// opens with. `Single` and `DisplayList` use one matrix in slot 0; `Multi` carries its own slot
// table and is what a skinned mesh uses.
enum class GuestMatrixGroupKind : std::uint8_t { Single, DisplayList, Multi };

[[nodiscard]] const char* guest_matrix_group_kind_name(GuestMatrixGroupKind kind) noexcept;

// GX's ten position/normal matrix registers, as a value. A matrix group loads some of them and
// inherits the rest, so this is carried from one group to the next in the order the title draws
// them -- across shapes as well as within one, because the hardware registers do not reset at a
// shape boundary either.
//
// It holds the resolved matrices rather than draw-matrix indices on purpose: an inherited slot
// refers to a matrix that was loaded out of a *previous* model's palette, so the index alone would
// be read against the wrong table.
class GuestMatrixRegisters {
  public:
    void load(std::uint32_t slot, const native_render::Matrix3x4& position,
              const native_render::Matrix3x4& normal, std::uint16_t drawMatrixIndex) noexcept;

    [[nodiscard]] bool loaded(std::uint32_t slot) const noexcept;
    [[nodiscard]] const native_render::Matrix3x4& position(std::uint32_t slot) const noexcept;
    [[nodiscard]] const native_render::Matrix3x4& normal(std::uint32_t slot) const noexcept;
    [[nodiscard]] std::uint16_t draw_matrix_index(std::uint32_t slot) const noexcept;

  private:
    std::array<native_render::Matrix3x4, kGuestMatrixSlotCount> positions_{};
    std::array<native_render::Matrix3x4, kGuestMatrixSlotCount> normals_{};
    // 0xffff is the one value a real draw-matrix index cannot take -- it is what the guest's own
    // slot table uses to mean "not this group" -- so it doubles as "never loaded".
    std::array<std::uint16_t, kGuestMatrixSlotCount> drawMatrixIndex_{
        kGuestUnusedMatrixIndex, kGuestUnusedMatrixIndex, kGuestUnusedMatrixIndex,
        kGuestUnusedMatrixIndex, kGuestUnusedMatrixIndex, kGuestUnusedMatrixIndex,
        kGuestUnusedMatrixIndex, kGuestUnusedMatrixIndex, kGuestUnusedMatrixIndex,
        kGuestUnusedMatrixIndex};
};

enum class GuestPoseError : std::uint8_t {
    None,
    NoReader,
    ElementOutOfRange,
    UnreadableShape,
    NoMatrixTable,
    UnreadableMatrixTable,
    NullMatrixGroup,
    UnreadableMatrixGroup,
    UnknownMatrixGroupKind,
    NoDrawMatrixData,
    UnreadableDrawMatrixData,
    NoViewNumber,
    UnreadableViewNumber,
    ViewNumberOutOfRange,
    NoDrawMatrixTable,
    UnreadableDrawMatrixTable,
    NoMatrixPalette,
    NoNormalMatrixTable,
    UnreadableNormalMatrixTable,
    NoNormalMatrixPalette,
    UnreadableNormalMatrix,
    UnreadableSlotTable,
    NoUsedMatrices,
    TooManyMatrices,
    MatrixIndexOutOfRange,
    UnreadableMatrix,
    UnreadableViewMatrix,
    InvalidMatrix,
};

[[nodiscard]] const char* guest_pose_error_name(GuestPoseError error) noexcept;

// One matrix group's pose, in the form `native-render` consumes.
//
// `pose` is the compact palette a `ModelDraw` carries. `slotToPoseIndex` maps a decoded vertex's
// `positionMatrixSlot` straight onto an index into it, with 0xFF for a slot the group never loads,
// so a display list referring to an unloaded slot is caught rather than drawn with whatever matrix
// happens to sit at that index.
struct GuestShapePose {
    GuestSkinningPipeline pipeline = GuestSkinningPipeline::IndexedPositionAndNormal;
    GuestMatrixGroupKind kind = GuestMatrixGroupKind::Single;
    GuestAddress matrixGroup = 0;
    std::uint32_t viewNumber = 0;
    // mDrawMtxData->mEntryNum: how many matrices the owning model's table holds. Every index this
    // group names has to fall inside it.
    std::uint32_t drawMatrixCount = 0;
    GuestAddress matrixPalette = 0;
    GuestAddress normalMatrixPalette = 0;
    // How many slots the group declares, including any it skips with 0xffff.
    std::uint32_t declaredSlotCount = 0;
    // How many of those it loaded itself, and how many of the pose's matrices it inherited from the
    // groups drawn before it. `pose.count` is the two together, which is every register a vertex of
    // this group may legitimately name.
    std::uint32_t loadedSlotCount = 0;
    std::uint32_t inheritedSlotCount = 0;
    // One bit per slot this group loaded itself. The rest of the pose came from earlier groups,
    // and telling the two apart is what says whether a vertex is drawn with this group's own
    // matrix or with one left behind.
    std::uint32_t loadedSlotMask = 0;
    native_render::ModelPose pose{};
    // j3dSys.mViewMtx as it stood when this group was drawn. Under the CPU pipelines it *is* the
    // position matrix; under the indexed ones it is not used, and is carried anyway because it is
    // the only way to tell a draw matrix that has the current view concatenated into it from one
    // left over from a pass that used another camera. Those two look identical in isolation.
    native_render::Matrix3x4 viewMatrix{};
    // The normal matrix behind each entry of `pose`, in the same order. J3D keeps a second palette
    // of 3x3 normal matrices alongside the position ones and, under the CPU pipelines, takes one of
    // the two from the view matrix while the other stays indexed -- so under PCPU and NCPU the two
    // palettes genuinely disagree and one matrix per slot cannot state both. It is carried as a 3x4
    // with a zero translation, which is what a 3x3 rotation means as a transform.
    //
    // `native_render::ModelDraw` has no place for it yet: its pose is positions only, and the
    // renderer derives normals from those. That is a real gap in the semantic boundary, named here
    // rather than hidden by publishing the position matrix twice.
    std::array<native_render::Matrix3x4, native_render::kMaxModelMatrices> normalViews{};
    std::array<std::uint8_t, kGuestMatrixSlotCount> slotToPoseIndex{};
    // The draw-matrix-table index behind each entry of `pose`, in the same order.
    std::array<std::uint16_t, native_render::kMaxModelMatrices> drawMatrixIndex{};
};

// GMSE01's three J3DShapeMtx vtables, read out of the retail image rather than assumed: the
// constructor J3DShapeFactory::newShapeMtx (0x802e8a84) stores 0x803e125c into the eight-byte
// object it allocates for shape types 0..2 and then 0x803e121c over it for the sixteen-byte type 3,
// which names the base and Multi tables. Scanning the image for the three known load overrides
// (J3DShapeMtx::load 0x802dfc04, J3DShapeMtxDL::load 0x802dfd14, J3DShapeMtxMulti::load 0x802dfd3c)
// finds them at 0x803e1274, 0x803e1254 and 0x803e1234, each exactly 0x18 past one of these three
// pointers -- so the display-list table is 0x803e123c, and the two independently derived tables
// agree.
constexpr GuestAddress GMSE01_J3D_SHAPE_MTX_VTABLE = 0x803e125c;
constexpr GuestAddress GMSE01_J3D_SHAPE_MTX_DL_VTABLE = 0x803e123c;
constexpr GuestAddress GMSE01_J3D_SHAPE_MTX_MULTI_VTABLE = 0x803e121c;

struct GuestMatrixGroupVtables {
    GuestAddress single = GMSE01_J3D_SHAPE_MTX_VTABLE;
    GuestAddress displayList = GMSE01_J3D_SHAPE_MTX_DL_VTABLE;
    GuestAddress multi = GMSE01_J3D_SHAPE_MTX_MULTI_VTABLE;
};

// `system` is the guest address of `j3dSys`, needed for its view matrix, which is the position
// matrix under the two CPU-position pipelines. `vtables` is a parameter for the same reason
// `system` is: so the tests drive this exact function rather than a copy of it.
//
// `registers` is read and written. Call this for each matrix group in the order the title draws
// them -- every group of a shape, then the next shape -- and carry the same registers across, or
// the inherited slots will be missing.
[[nodiscard]] GuestPoseError
read_guest_shape_pose(const GuestMemory& memory, const GuestShape& shape, std::uint16_t element,
                      GuestAddress system, const GuestMatrixGroupVtables& vtables,
                      GuestMatrixRegisters& registers, GuestShapePose& out) noexcept;

} // namespace sb::title_adapter
