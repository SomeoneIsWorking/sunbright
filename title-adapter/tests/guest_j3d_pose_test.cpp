// Drives the shipping pose reader against a synthetic guest image.
//
// The pose is where a wrong answer is hardest to see: every matrix is twelve plausible floats, so a
// reader that took the wrong palette, the wrong view, or the wrong matrix for the pipeline still
// produces a pose that looks like a pose. The fixture is built so each of those mistakes lands on a
// distinct, recognisable matrix instead.

#include <sunbright/title_adapter/guest_j3d_pose.h>

#include "guest_image.h"

#include <array>
#include <cassert>
#include <string_view>

namespace {

using sb::title_adapter::GuestAddress;
using sb::title_adapter::GuestMatrixGroupKind;
using sb::title_adapter::GuestMatrixGroupVtables;
using sb::title_adapter::GuestMatrixRegisters;
using sb::title_adapter::GuestMemory;
using sb::title_adapter::GuestPoseError;
using sb::title_adapter::GuestShape;
using sb::title_adapter::GuestShapePose;
using sb::title_adapter::GuestSkinningPipeline;
using sb::title_adapter::test::Image;
using sb::title_adapter::test::read_image;

constexpr GuestAddress SHAPE = 0x80000100;
constexpr GuestAddress SYSTEM = 0x80000300;
constexpr GuestAddress MATRIX_TABLE = 0x80000400;
constexpr GuestAddress SINGLE_GROUP = 0x80000420;
constexpr GuestAddress MULTI_GROUP = 0x80000440;
constexpr GuestAddress MULTI_SLOTS = 0x80000460;
constexpr GuestAddress DRAW_MTX_DATA = 0x80000480;
constexpr GuestAddress DRAW_MATRIX_TABLE = 0x800004a0;
constexpr GuestAddress NORM_MATRIX_TABLE = 0x800004c0;
constexpr GuestAddress VIEW_NUMBER = 0x800004e0;
// Two views of each palette, so taking view 0 when the shape says view 1 is a wrong answer rather
// than an accidentally right one.
constexpr GuestAddress DRAW_PALETTE_VIEW0 = 0x80000500;
constexpr GuestAddress DRAW_PALETTE_VIEW1 = 0x80000600;
constexpr GuestAddress NORM_PALETTE_VIEW0 = 0x80000700;
constexpr GuestAddress NORM_PALETTE_VIEW1 = 0x80000800;

constexpr std::uint32_t DRAW_MATRIX_COUNT = 6;
constexpr std::uint32_t MATRIX_BYTES = 48;
constexpr std::uint32_t NORMAL_MATRIX_BYTES = 36;

constexpr GuestMatrixGroupVtables VTABLES{};

// Every matrix in the image is the identity except for one marker value that says exactly which
// matrix it is: palette, view and draw-matrix index are all recoverable from the number that comes
// back.
constexpr float VIEW_MARKER = 1000.0F;
constexpr float NORMAL_VIEW_MARKER = 2000.0F;

[[nodiscard]] constexpr float draw_marker(std::uint32_t view, std::uint32_t index) {
    return static_cast<float>(view * 100U + index);
}

[[nodiscard]] constexpr float normal_marker(std::uint32_t view, std::uint32_t index) {
    return static_cast<float>(500U + view * 100U + index);
}

void write_matrix(Image& image, GuestAddress address, float marker) {
    image.real(address + 0x00, 1.0F);
    image.real(address + 0x14, 1.0F);
    image.real(address + 0x28, 1.0F);
    // The translation column, which a 3x3 normal matrix has no room for.
    image.real(address + 0x0c, marker);
}

void write_normal_matrix(Image& image, GuestAddress address, float marker) {
    image.real(address + 0x00, 1.0F);
    image.real(address + 0x10, 1.0F);
    image.real(address + 0x20, 1.0F);
    image.real(address + 0x04, marker);
}

void build(Image& image) {
    image.half(SHAPE + 0x06, 2);                 // mElementCount
    image.word(SHAPE + 0x08, 0x00000001);        // mFlags: Visible, pipeline 0
    image.word(SHAPE + 0x34, MATRIX_TABLE);      // mMatrices
    image.word(SHAPE + 0x48, DRAW_MTX_DATA);     // mDrawMtxData
    image.word(SHAPE + 0x50, DRAW_MATRIX_TABLE); // mDrawMatrices
    image.word(SHAPE + 0x54, NORM_MATRIX_TABLE); // mNormMatrices
    image.word(SHAPE + 0x58, VIEW_NUMBER);       // mCurrentViewNo

    image.half(DRAW_MTX_DATA + 0x00, static_cast<std::uint16_t>(DRAW_MATRIX_COUNT));
    image.word(VIEW_NUMBER, 1);

    image.word(DRAW_MATRIX_TABLE + 0x00, DRAW_PALETTE_VIEW0);
    image.word(DRAW_MATRIX_TABLE + 0x04, DRAW_PALETTE_VIEW1);
    image.word(NORM_MATRIX_TABLE + 0x00, NORM_PALETTE_VIEW0);
    image.word(NORM_MATRIX_TABLE + 0x04, NORM_PALETTE_VIEW1);

    for (std::uint32_t index = 0; index < DRAW_MATRIX_COUNT; ++index) {
        write_matrix(image, DRAW_PALETTE_VIEW0 + index * MATRIX_BYTES, draw_marker(0, index));
        write_matrix(image, DRAW_PALETTE_VIEW1 + index * MATRIX_BYTES, draw_marker(1, index));
        write_normal_matrix(image, NORM_PALETTE_VIEW0 + index * NORMAL_MATRIX_BYTES,
                            normal_marker(0, index));
        write_normal_matrix(image, NORM_PALETTE_VIEW1 + index * NORMAL_MATRIX_BYTES,
                            normal_marker(1, index));
    }

    write_matrix(image, SYSTEM + 0x00, VIEW_MARKER); // j3dSys.mViewMtx
    image.real(SYSTEM + 0x04, NORMAL_VIEW_MARKER);   // a rotation term, to prove it survives

    image.word(MATRIX_TABLE + 0x00, SINGLE_GROUP);
    image.word(MATRIX_TABLE + 0x04, MULTI_GROUP);

    image.word(SINGLE_GROUP + 0x00, VTABLES.single);
    image.half(SINGLE_GROUP + 0x04, 4); // unk4: draw-matrix index

    image.word(MULTI_GROUP + 0x00, VTABLES.multi);
    image.half(MULTI_GROUP + 0x04, 0xffff); // unk4 is unused by Multi; a trap value proves it
    image.half(MULTI_GROUP + 0x08, 3);      // unk8: declared slot count
    image.word(MULTI_GROUP + 0x0c, MULTI_SLOTS);
    image.half(MULTI_SLOTS + 0x00, 2);
    image.half(MULTI_SLOTS + 0x02, 0xffff); // a hole: this slot is never loaded
    image.half(MULTI_SLOTS + 0x04, 5);
}

[[nodiscard]] float marker_of(const sb::native_render::Matrix3x4& matrix) {
    return matrix.value[3];
}

[[nodiscard]] float normal_marker_of(const sb::native_render::Matrix3x4& matrix) {
    return matrix.value[1];
}

GuestShape shape_with_flags(std::uint32_t flags) {
    GuestShape shape{};
    shape.address = SHAPE;
    shape.elementCount = 2;
    shape.flags = flags;
    return shape;
}

void reads_a_single_matrix_group() {
    Image image;
    build(image);
    const GuestMemory memory{read_image, &image};

    GuestMatrixRegisters registers{};
    GuestShapePose pose{};
    assert(read_guest_shape_pose(memory, shape_with_flags(0x1), 0, SYSTEM, VTABLES, registers,
                                 pose) == GuestPoseError::None);
    assert(pose.kind == GuestMatrixGroupKind::Single);
    assert(pose.pipeline == GuestSkinningPipeline::IndexedPositionAndNormal);
    assert(pose.matrixGroup == SINGLE_GROUP);
    assert(pose.viewNumber == 1);
    assert(pose.drawMatrixCount == DRAW_MATRIX_COUNT);
    assert(pose.matrixPalette == DRAW_PALETTE_VIEW1);
    assert(pose.normalMatrixPalette == NORM_PALETTE_VIEW1);
    assert(pose.declaredSlotCount == 1);
    assert(pose.pose.count == 1);
    assert(pose.drawMatrixIndex[0] == 4);
    // View 1, matrix 4 -- not view 0, and not any neighbouring matrix.
    assert(marker_of(pose.pose.modelViews[0]) == draw_marker(1, 4));
    assert(normal_marker_of(pose.normalViews[0]) == normal_marker(1, 4));
    // Slot 0 is GX matrix register 0, and nothing else is loaded.
    assert(pose.slotToPoseIndex[0] == 0);
    for (std::uint32_t slot = 1; slot < sb::title_adapter::kGuestMatrixSlotCount; ++slot) {
        assert(pose.slotToPoseIndex[slot] == 0xff);
    }
}

void reads_a_skinned_matrix_group() {
    Image image;
    build(image);
    const GuestMemory memory{read_image, &image};

    GuestMatrixRegisters registers{};
    GuestShapePose pose{};
    assert(read_guest_shape_pose(memory, shape_with_flags(0x1), 1, SYSTEM, VTABLES, registers,
                                 pose) == GuestPoseError::None);
    assert(pose.kind == GuestMatrixGroupKind::Multi);
    assert(pose.matrixGroup == MULTI_GROUP);
    // Three slots declared, one of them a 0xffff hole, so two matrices are loaded. Nothing was
    // drawn before it here, so the hole stays empty and the pose holds only those two.
    assert(pose.declaredSlotCount == 3);
    assert(pose.loadedSlotCount == 2);
    assert(pose.inheritedSlotCount == 0);
    assert(pose.pose.count == 2);
    assert(pose.drawMatrixIndex[0] == 2);
    assert(pose.drawMatrixIndex[1] == 5);
    assert(marker_of(pose.pose.modelViews[0]) == draw_marker(1, 2));
    assert(marker_of(pose.pose.modelViews[1]) == draw_marker(1, 5));
    assert(normal_marker_of(pose.normalViews[0]) == normal_marker(1, 2));
    assert(normal_marker_of(pose.normalViews[1]) == normal_marker(1, 5));
    // The skipped slot stays unused rather than shifting the two that follow it down onto the
    // wrong matrices, which is what a compaction that ignored the hole would do.
    assert(pose.slotToPoseIndex[0] == 0);
    assert(pose.slotToPoseIndex[1] == 0xff);
    assert(pose.slotToPoseIndex[2] == 1);
    assert(pose.slotToPoseIndex[3] == 0xff);
}

// A 0xffff slot is inherited, not empty: GMSE01's own matrix groups leave gaps between the slots
// they fill and then draw through them, which only works because GX keeps what the previous group
// loaded. A reader that starts each group from nothing reports those vertices as drawn through an
// unloaded slot -- measured on the real title as 276,696 of 11.4 million before this was modelled.
void inherits_slots_an_earlier_group_loaded() {
    Image image;
    build(image);
    // Move the skinned group's hole onto slot 0, the slot the single group fills, so that the
    // inherited matrix is one it never names itself.
    image.half(MULTI_SLOTS + 0x00, 0xffff);
    image.half(MULTI_SLOTS + 0x02, 2);
    image.half(MULTI_SLOTS + 0x04, 5);
    const GuestMemory memory{read_image, &image};

    GuestMatrixRegisters registers{};
    GuestShapePose first{};
    // The single group fills slot 0 with draw matrix 4.
    assert(read_guest_shape_pose(memory, shape_with_flags(0x1), 0, SYSTEM, VTABLES, registers,
                                 first) == GuestPoseError::None);
    assert(first.pose.count == 1);

    // The skinned group declares slot 0 as a hole and fills slots 1 and 2. Slot 0 is still in its
    // pose, carried over from the group before it.
    GuestShapePose second{};
    assert(read_guest_shape_pose(memory, shape_with_flags(0x1), 1, SYSTEM, VTABLES, registers,
                                 second) == GuestPoseError::None);
    assert(second.loadedSlotCount == 2);
    assert(second.inheritedSlotCount == 1);
    assert(second.pose.count == 3);
    assert(second.slotToPoseIndex[0] == 0);
    assert(second.slotToPoseIndex[1] == 1);
    assert(second.slotToPoseIndex[2] == 2);
    // The inherited slot still holds the matrix the earlier group put there, not a fresh read of
    // draw matrix 0, which is what an index carried across instead of a matrix would have given.
    assert(marker_of(second.pose.modelViews[0]) == draw_marker(1, 4));
    assert(second.drawMatrixIndex[0] == 4);
    assert(marker_of(second.pose.modelViews[1]) == draw_marker(1, 2));
    assert(marker_of(second.pose.modelViews[2]) == draw_marker(1, 5));
}

// The four pipelines differ only in which matrix a slot ends up holding, which is exactly the kind
// of difference that disappears if it is not asserted. Each case names both matrices.
void honours_every_skinning_pipeline() {
    Image image;
    build(image);
    const GuestMemory memory{read_image, &image};
    GuestShapePose pose{};

    // A fresh set of registers per case, so each pipeline's answer is its own rather than a matrix
    // left behind by the case before it.
    GuestMatrixRegisters registers{};

    // PNGP: both indexed.
    assert(read_guest_shape_pose(memory, shape_with_flags(0x0), 0, SYSTEM, VTABLES, registers,
                                 pose) == GuestPoseError::None);
    assert(pose.pipeline == GuestSkinningPipeline::IndexedPositionAndNormal);
    assert(marker_of(pose.pose.modelViews[0]) == draw_marker(1, 4));
    assert(normal_marker_of(pose.normalViews[0]) == normal_marker(1, 4));

    // PCPU (J3DShpFlag_SkinPosCpu): positions are already in view space, normals stay indexed.
    assert(read_guest_shape_pose(memory, shape_with_flags(0x4), 0, SYSTEM, VTABLES, registers,
                                 pose) == GuestPoseError::None);
    assert(pose.pipeline == GuestSkinningPipeline::CpuPosition);
    assert(marker_of(pose.pose.modelViews[0]) == VIEW_MARKER);
    assert(normal_marker_of(pose.normalViews[0]) == normal_marker(1, 4));

    // NCPU (J3DShpFlag_SkinNrmCpu): the other way round.
    assert(read_guest_shape_pose(memory, shape_with_flags(0x8), 0, SYSTEM, VTABLES, registers,
                                 pose) == GuestPoseError::None);
    assert(pose.pipeline == GuestSkinningPipeline::CpuNormal);
    assert(marker_of(pose.pose.modelViews[0]) == draw_marker(1, 4));
    assert(normal_marker_of(pose.normalViews[0]) == NORMAL_VIEW_MARKER);

    // PNCPU: both from the view matrix, and the normal matrix drops its translation.
    assert(read_guest_shape_pose(memory, shape_with_flags(0xc), 0, SYSTEM, VTABLES, registers,
                                 pose) == GuestPoseError::None);
    assert(pose.pipeline == GuestSkinningPipeline::CpuPositionAndNormal);
    assert(marker_of(pose.pose.modelViews[0]) == VIEW_MARKER);
    assert(normal_marker_of(pose.normalViews[0]) == NORMAL_VIEW_MARKER);
    assert(marker_of(pose.normalViews[0]) == 0.0F);
}

void refuses_what_it_cannot_read() {
    Image image;
    build(image);
    const GuestMemory memory{read_image, &image};
    const GuestShape shape = shape_with_flags(0x1);
    GuestMatrixRegisters registers{};
    GuestShapePose pose{};

    assert(read_guest_shape_pose({nullptr, nullptr}, shape, 0, SYSTEM, VTABLES, registers, pose) ==
           GuestPoseError::NoReader);
    assert(read_guest_shape_pose(memory, shape, 2, SYSTEM, VTABLES, registers, pose) ==
           GuestPoseError::ElementOutOfRange);

    GuestShape unmapped = shape;
    unmapped.address = 0x00001000;
    assert(read_guest_shape_pose(memory, unmapped, 0, SYSTEM, VTABLES, registers, pose) ==
           GuestPoseError::UnreadableShape);

    struct Missing {
        GuestAddress field;
        GuestPoseError error;
    };
    constexpr std::array missing = {
        Missing{0x34, GuestPoseError::NoMatrixTable},
        Missing{0x48, GuestPoseError::NoDrawMatrixData},
        Missing{0x50, GuestPoseError::NoDrawMatrixTable},
        Missing{0x54, GuestPoseError::NoNormalMatrixTable},
        Missing{0x58, GuestPoseError::NoViewNumber},
    };
    for (const Missing& entry : missing) {
        Image broken = image;
        broken.word(SHAPE + entry.field, 0);
        const GuestMemory brokenMemory{read_image, &broken};
        assert(read_guest_shape_pose(brokenMemory, shape, 0, SYSTEM, VTABLES, registers, pose) ==
               entry.error);
    }

    Image farView = image;
    farView.word(VIEW_NUMBER, 64);
    const GuestMemory farViewMemory{read_image, &farView};
    assert(read_guest_shape_pose(farViewMemory, shape, 0, SYSTEM, VTABLES, registers, pose) ==
           GuestPoseError::ViewNumberOutOfRange);

    Image noPalette = image;
    noPalette.word(DRAW_MATRIX_TABLE + 0x04, 0);
    const GuestMemory noPaletteMemory{read_image, &noPalette};
    assert(read_guest_shape_pose(noPaletteMemory, shape, 0, SYSTEM, VTABLES, registers, pose) ==
           GuestPoseError::NoMatrixPalette);

    Image noNormalPalette = image;
    noNormalPalette.word(NORM_MATRIX_TABLE + 0x04, 0);
    const GuestMemory noNormalPaletteMemory{read_image, &noNormalPalette};
    assert(read_guest_shape_pose(noNormalPaletteMemory, shape, 0, SYSTEM, VTABLES, registers,
                                 pose) == GuestPoseError::NoNormalMatrixPalette);

    Image nullGroup = image;
    nullGroup.word(MATRIX_TABLE + 0x00, 0);
    const GuestMemory nullGroupMemory{read_image, &nullGroup};
    assert(read_guest_shape_pose(nullGroupMemory, shape, 0, SYSTEM, VTABLES, registers, pose) ==
           GuestPoseError::NullMatrixGroup);

    // A vtable this build does not know is not a shape matrix. Guessing its layout would read the
    // wrong fields and answer with a pose.
    Image strangeKind = image;
    strangeKind.word(SINGLE_GROUP + 0x00, 0x80001234);
    const GuestMemory strangeKindMemory{read_image, &strangeKind};
    assert(read_guest_shape_pose(strangeKindMemory, shape, 0, SYSTEM, VTABLES, registers, pose) ==
           GuestPoseError::UnknownMatrixGroupKind);

    // An index past the model's own draw-matrix table would read whatever follows the palette.
    Image farIndex = image;
    farIndex.half(SINGLE_GROUP + 0x04, static_cast<std::uint16_t>(DRAW_MATRIX_COUNT));
    const GuestMemory farIndexMemory{read_image, &farIndex};
    assert(read_guest_shape_pose(farIndexMemory, shape, 0, SYSTEM, VTABLES, registers, pose) ==
           GuestPoseError::MatrixIndexOutOfRange);

    Image tooMany = image;
    tooMany.half(MULTI_GROUP + 0x08, sb::native_render::kMaxModelMatrices + 1);
    const GuestMemory tooManyMemory{read_image, &tooMany};
    assert(read_guest_shape_pose(tooManyMemory, shape, 1, SYSTEM, VTABLES, registers, pose) ==
           GuestPoseError::TooManyMatrices);

    // A group every one of whose slots is a hole loads no matrix at all, which is not a pose.
    Image allHoles = image;
    allHoles.half(MULTI_SLOTS + 0x00, 0xffff);
    allHoles.half(MULTI_SLOTS + 0x04, 0xffff);
    const GuestMemory allHolesMemory{read_image, &allHoles};
    assert(read_guest_shape_pose(allHolesMemory, shape, 1, SYSTEM, VTABLES, registers, pose) ==
           GuestPoseError::NoUsedMatrices);

    // A matrix the game left as a NaN is refused rather than published into a draw.
    Image notANumber = image;
    notANumber.word(DRAW_PALETTE_VIEW1 + 4 * MATRIX_BYTES, 0x7fc00000);
    const GuestMemory notANumberMemory{read_image, &notANumber};
    assert(read_guest_shape_pose(notANumberMemory, shape, 0, SYSTEM, VTABLES, registers, pose) ==
           GuestPoseError::InvalidMatrix);
}

void names_every_error() {
    constexpr std::array errors = {
        GuestPoseError::None,
        GuestPoseError::NoReader,
        GuestPoseError::ElementOutOfRange,
        GuestPoseError::UnreadableShape,
        GuestPoseError::NoMatrixTable,
        GuestPoseError::UnreadableMatrixTable,
        GuestPoseError::NullMatrixGroup,
        GuestPoseError::UnreadableMatrixGroup,
        GuestPoseError::UnknownMatrixGroupKind,
        GuestPoseError::NoDrawMatrixData,
        GuestPoseError::UnreadableDrawMatrixData,
        GuestPoseError::NoViewNumber,
        GuestPoseError::UnreadableViewNumber,
        GuestPoseError::ViewNumberOutOfRange,
        GuestPoseError::NoDrawMatrixTable,
        GuestPoseError::UnreadableDrawMatrixTable,
        GuestPoseError::NoMatrixPalette,
        GuestPoseError::NoNormalMatrixTable,
        GuestPoseError::UnreadableNormalMatrixTable,
        GuestPoseError::NoNormalMatrixPalette,
        GuestPoseError::UnreadableNormalMatrix,
        GuestPoseError::UnreadableSlotTable,
        GuestPoseError::NoUsedMatrices,
        GuestPoseError::TooManyMatrices,
        GuestPoseError::MatrixIndexOutOfRange,
        GuestPoseError::UnreadableMatrix,
        GuestPoseError::UnreadableViewMatrix,
        GuestPoseError::InvalidMatrix,
    };
    for (const GuestPoseError error : errors) {
        assert(std::string_view(guest_pose_error_name(error)) != "unknown");
    }
    constexpr std::array pipelines = {
        GuestSkinningPipeline::IndexedPositionAndNormal,
        GuestSkinningPipeline::CpuPosition,
        GuestSkinningPipeline::CpuNormal,
        GuestSkinningPipeline::CpuPositionAndNormal,
    };
    for (const GuestSkinningPipeline pipeline : pipelines) {
        assert(std::string_view(guest_skinning_pipeline_name(pipeline)) != "unknown");
    }
    constexpr std::array kinds = {
        GuestMatrixGroupKind::Single,
        GuestMatrixGroupKind::DisplayList,
        GuestMatrixGroupKind::Multi,
    };
    for (const GuestMatrixGroupKind kind : kinds) {
        assert(std::string_view(guest_matrix_group_kind_name(kind)) != "unknown");
    }
}

} // namespace

int main() {
    reads_a_single_matrix_group();
    reads_a_skinned_matrix_group();
    inherits_slots_an_earlier_group_loaded();
    honours_every_skinning_pipeline();
    refuses_what_it_cannot_read();
    names_every_error();
    return 0;
}
