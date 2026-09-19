#include <sunbright/title_adapter/guest_j3d_pose.h>

#include <algorithm>
#include <array>

namespace sb::title_adapter {
namespace {

// Retail J3DShape, from decomp/sms/include/JSystem/J3D/J3DGraphBase/J3DShape.hpp.
constexpr GuestAddress SHAPE_MATRICES = 0x34;
constexpr GuestAddress SHAPE_DRAW_MTX_DATA = 0x48;
constexpr GuestAddress SHAPE_DRAW_MATRICES = 0x50;
constexpr GuestAddress SHAPE_NORM_MATRICES = 0x54;
constexpr GuestAddress SHAPE_CURRENT_VIEW_NO = 0x58;

// Retail J3DShapeMtx and J3DShapeMtxMulti. Both open with the vtable pointer their virtual
// destructor requires, so the declared members follow it.
constexpr GuestAddress MATRIX_GROUP_VTABLE = 0x00;
constexpr GuestAddress MATRIX_GROUP_INDEX = 0x04;
constexpr GuestAddress MATRIX_GROUP_MULTI_COUNT = 0x08;
constexpr GuestAddress MATRIX_GROUP_MULTI_TABLE = 0x0c;

// Retail J3DDrawMtxData, from decomp/sms/include/JSystem/J3D/J3DGraphBase/J3DVertex.hpp.
constexpr GuestAddress DRAW_MTX_DATA_ENTRY_NUM = 0x00;

// A GameCube `Mtx` is a row-major 3x4 of f32, which is also how native_render::Matrix3x4 stores it,
// so the twelve values cross in order and only their byte order changes.
constexpr std::uint32_t MATRIX_FLOATS = 12;
constexpr std::uint32_t MATRIX_BYTES = MATRIX_FLOATS * 4;

// A GameCube `Mtx33` is a row-major 3x3 of f32. It is widened here into the 3x4 the renderer's
// matrix type is, by leaving the fourth column zero: a normal matrix has no translation.
constexpr std::uint32_t NORMAL_MATRIX_ROWS = 3;
constexpr std::uint32_t NORMAL_MATRIX_BYTES = NORMAL_MATRIX_ROWS * NORMAL_MATRIX_ROWS * 4;

// J3DSys::mViewMtx is the first member of the system object.
constexpr GuestAddress SYSTEM_VIEW_MTX = 0x00;

// mCurrentViewNo indexes the model's per-view draw-matrix buffers, which J3DModel allocates one of
// per view (J3DModel.cpp:550). Authored view counts are one or a small handful; this cap is far
// above any of them, so reaching it means mCurrentViewNo is not pointing at a view number and the
// pointer read that followed would be an arbitrary word of guest memory.
constexpr std::uint32_t MAX_VIEW_NUMBER = 8;

[[nodiscard]] bool read_matrix(const GuestReader& reader, GuestAddress address,
                               native_render::Matrix3x4& matrix) {
    for (std::uint32_t element = 0; element < MATRIX_FLOATS; ++element) {
        if (!reader.real(address + element * 4, matrix.value[element])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool read_normal_matrix(const GuestReader& reader, GuestAddress address,
                                      native_render::Matrix3x4& matrix) {
    matrix = {};
    for (std::uint32_t row = 0; row < NORMAL_MATRIX_ROWS; ++row) {
        for (std::uint32_t column = 0; column < NORMAL_MATRIX_ROWS; ++column) {
            const GuestAddress element = address + (row * NORMAL_MATRIX_ROWS + column) * 4;
            if (!reader.real(element, matrix.value[row * 4 + column])) {
                return false;
            }
        }
    }
    return true;
}

// The rotation part of a 3x4, with its translation dropped: what the view matrix means when
// J3DShapeMtx::load uses it as a normal matrix.
[[nodiscard]] native_render::Matrix3x4 rotation_of(const native_render::Matrix3x4& matrix) {
    native_render::Matrix3x4 rotation = matrix;
    rotation.value[3] = 0.0F;
    rotation.value[7] = 0.0F;
    rotation.value[11] = 0.0F;
    return rotation;
}

[[nodiscard]] GuestPoseError classify(const GuestReader& reader, GuestAddress group,
                                      const GuestMatrixGroupVtables& vtables,
                                      GuestMatrixGroupKind& kind) {
    GuestAddress vtable = 0;
    if (!reader.word(group + MATRIX_GROUP_VTABLE, vtable)) {
        return GuestPoseError::UnreadableMatrixGroup;
    }
    if (vtable == vtables.single) {
        kind = GuestMatrixGroupKind::Single;
        return GuestPoseError::None;
    }
    if (vtable == vtables.displayList) {
        kind = GuestMatrixGroupKind::DisplayList;
        return GuestPoseError::None;
    }
    if (vtable == vtables.multi) {
        kind = GuestMatrixGroupKind::Multi;
        return GuestPoseError::None;
    }
    return GuestPoseError::UnknownMatrixGroupKind;
}

// The draw-matrix-table indices this group loads, one per GX slot, with 0xffff where
// J3DShapeMtxMulti::load skips a slot.
[[nodiscard]] GuestPoseError
read_slot_indices(const GuestReader& reader, GuestAddress group, GuestMatrixGroupKind kind,
                  std::array<std::uint16_t, native_render::kMaxModelMatrices>& indices,
                  std::uint32_t& count) {
    if (kind != GuestMatrixGroupKind::Multi) {
        // J3DShapeMtx::load and J3DShapeMtxDL both load one matrix into slot 0.
        if (!reader.half(group + MATRIX_GROUP_INDEX, indices[0])) {
            return GuestPoseError::UnreadableMatrixGroup;
        }
        count = 1;
        return GuestPoseError::None;
    }

    std::uint16_t declared = 0;
    GuestAddress table = 0;
    if (!reader.half(group + MATRIX_GROUP_MULTI_COUNT, declared) ||
        !reader.word(group + MATRIX_GROUP_MULTI_TABLE, table)) {
        return GuestPoseError::UnreadableMatrixGroup;
    }
    if (declared > native_render::kMaxModelMatrices) {
        return GuestPoseError::TooManyMatrices;
    }
    if (table == 0) {
        return GuestPoseError::UnreadableSlotTable;
    }
    for (std::uint16_t slot = 0; slot < declared; ++slot) {
        if (!reader.half(table + static_cast<std::uint32_t>(slot) * 2, indices[slot])) {
            return GuestPoseError::UnreadableSlotTable;
        }
    }
    count = declared;
    return GuestPoseError::None;
}

} // namespace

void GuestMatrixRegisters::load(std::uint32_t slot, const native_render::Matrix3x4& position,
                                const native_render::Matrix3x4& normal,
                                std::uint16_t drawMatrixIndex) noexcept {
    positions_[slot] = position;
    normals_[slot] = normal;
    drawMatrixIndex_[slot] = drawMatrixIndex;
}

bool GuestMatrixRegisters::loaded(std::uint32_t slot) const noexcept {
    return drawMatrixIndex_[slot] != kGuestUnusedMatrixIndex;
}

const native_render::Matrix3x4& GuestMatrixRegisters::position(std::uint32_t slot) const noexcept {
    return positions_[slot];
}

const native_render::Matrix3x4& GuestMatrixRegisters::normal(std::uint32_t slot) const noexcept {
    return normals_[slot];
}

std::uint16_t GuestMatrixRegisters::draw_matrix_index(std::uint32_t slot) const noexcept {
    return drawMatrixIndex_[slot];
}

const char* guest_skinning_pipeline_name(GuestSkinningPipeline pipeline) noexcept {
    switch (pipeline) {
    case GuestSkinningPipeline::IndexedPositionAndNormal:
        return "indexed_position_and_normal";
    case GuestSkinningPipeline::CpuPosition:
        return "cpu_position";
    case GuestSkinningPipeline::CpuNormal:
        return "cpu_normal";
    case GuestSkinningPipeline::CpuPositionAndNormal:
        return "cpu_position_and_normal";
    }
    return "unknown";
}

const char* guest_matrix_group_kind_name(GuestMatrixGroupKind kind) noexcept {
    switch (kind) {
    case GuestMatrixGroupKind::Single:
        return "single";
    case GuestMatrixGroupKind::DisplayList:
        return "display_list";
    case GuestMatrixGroupKind::Multi:
        return "multi";
    }
    return "unknown";
}

const char* guest_pose_error_name(GuestPoseError error) noexcept {
    switch (error) {
    case GuestPoseError::None:
        return "none";
    case GuestPoseError::NoReader:
        return "no_reader";
    case GuestPoseError::ElementOutOfRange:
        return "element_out_of_range";
    case GuestPoseError::UnreadableShape:
        return "unreadable_shape";
    case GuestPoseError::NoMatrixTable:
        return "no_matrix_table";
    case GuestPoseError::UnreadableMatrixTable:
        return "unreadable_matrix_table";
    case GuestPoseError::NullMatrixGroup:
        return "null_matrix_group";
    case GuestPoseError::UnreadableMatrixGroup:
        return "unreadable_matrix_group";
    case GuestPoseError::UnknownMatrixGroupKind:
        return "unknown_matrix_group_kind";
    case GuestPoseError::NoDrawMatrixData:
        return "no_draw_matrix_data";
    case GuestPoseError::UnreadableDrawMatrixData:
        return "unreadable_draw_matrix_data";
    case GuestPoseError::NoViewNumber:
        return "no_view_number";
    case GuestPoseError::UnreadableViewNumber:
        return "unreadable_view_number";
    case GuestPoseError::ViewNumberOutOfRange:
        return "view_number_out_of_range";
    case GuestPoseError::NoDrawMatrixTable:
        return "no_draw_matrix_table";
    case GuestPoseError::UnreadableDrawMatrixTable:
        return "unreadable_draw_matrix_table";
    case GuestPoseError::NoMatrixPalette:
        return "no_matrix_palette";
    case GuestPoseError::NoNormalMatrixTable:
        return "no_normal_matrix_table";
    case GuestPoseError::UnreadableNormalMatrixTable:
        return "unreadable_normal_matrix_table";
    case GuestPoseError::NoNormalMatrixPalette:
        return "no_normal_matrix_palette";
    case GuestPoseError::UnreadableNormalMatrix:
        return "unreadable_normal_matrix";
    case GuestPoseError::UnreadableSlotTable:
        return "unreadable_slot_table";
    case GuestPoseError::NoUsedMatrices:
        return "no_used_matrices";
    case GuestPoseError::TooManyMatrices:
        return "too_many_matrices";
    case GuestPoseError::MatrixIndexOutOfRange:
        return "matrix_index_out_of_range";
    case GuestPoseError::UnreadableMatrix:
        return "unreadable_matrix";
    case GuestPoseError::UnreadableViewMatrix:
        return "unreadable_view_matrix";
    case GuestPoseError::InvalidMatrix:
        return "invalid_matrix";
    }
    return "unknown";
}

GuestPoseError read_guest_shape_pose(const GuestMemory& memory, const GuestShape& shape,
                                     std::uint16_t element, GuestAddress system,
                                     const GuestMatrixGroupVtables& vtables,
                                     GuestMatrixRegisters& registers,
                                     GuestShapePose& out) noexcept {
    if (memory.read == nullptr) {
        return GuestPoseError::NoReader;
    }
    if (element >= shape.elementCount) {
        return GuestPoseError::ElementOutOfRange;
    }
    const GuestReader reader(memory);

    GuestShapePose result{};
    std::ranges::fill(result.slotToPoseIndex, 0xffU);
    std::ranges::fill(result.drawMatrixIndex, kGuestUnusedMatrixIndex);
    result.pipeline = static_cast<GuestSkinningPipeline>((shape.flags >> 2) & 3U);

    GuestAddress matrixTable = 0;
    GuestAddress drawMatrixData = 0;
    GuestAddress drawMatrixTable = 0;
    GuestAddress normalMatrixTable = 0;
    GuestAddress viewNumber = 0;
    if (!reader.word(shape.address + SHAPE_MATRICES, matrixTable) ||
        !reader.word(shape.address + SHAPE_DRAW_MTX_DATA, drawMatrixData) ||
        !reader.word(shape.address + SHAPE_DRAW_MATRICES, drawMatrixTable) ||
        !reader.word(shape.address + SHAPE_NORM_MATRICES, normalMatrixTable) ||
        !reader.word(shape.address + SHAPE_CURRENT_VIEW_NO, viewNumber)) {
        return GuestPoseError::UnreadableShape;
    }
    if (matrixTable == 0) {
        return GuestPoseError::NoMatrixTable;
    }
    if (drawMatrixData == 0) {
        return GuestPoseError::NoDrawMatrixData;
    }
    // J3DShape::draw returns before drawing at all when these are still null, which is how a shape
    // whose owning model has not been updated yet reaches its draw. Naming the two separately keeps
    // that legitimate not-yet state distinguishable from a corrupt shape.
    if (drawMatrixTable == 0) {
        return GuestPoseError::NoDrawMatrixTable;
    }
    if (normalMatrixTable == 0) {
        return GuestPoseError::NoNormalMatrixTable;
    }
    if (viewNumber == 0) {
        return GuestPoseError::NoViewNumber;
    }

    std::uint16_t entryNumber = 0;
    if (!reader.half(drawMatrixData + DRAW_MTX_DATA_ENTRY_NUM, entryNumber)) {
        return GuestPoseError::UnreadableDrawMatrixData;
    }
    result.drawMatrixCount = entryNumber;

    if (!reader.word(viewNumber, result.viewNumber)) {
        return GuestPoseError::UnreadableViewNumber;
    }
    if (result.viewNumber >= MAX_VIEW_NUMBER) {
        return GuestPoseError::ViewNumberOutOfRange;
    }
    if (!reader.word(drawMatrixTable + result.viewNumber * 4, result.matrixPalette)) {
        return GuestPoseError::UnreadableDrawMatrixTable;
    }
    if (result.matrixPalette == 0) {
        return GuestPoseError::NoMatrixPalette;
    }
    if (!reader.word(normalMatrixTable + result.viewNumber * 4, result.normalMatrixPalette)) {
        return GuestPoseError::UnreadableNormalMatrixTable;
    }
    if (result.normalMatrixPalette == 0) {
        return GuestPoseError::NoNormalMatrixPalette;
    }

    if (!reader.word(matrixTable + static_cast<std::uint32_t>(element) * 4, result.matrixGroup)) {
        return GuestPoseError::UnreadableMatrixTable;
    }
    if (result.matrixGroup == 0) {
        return GuestPoseError::NullMatrixGroup;
    }
    if (const GuestPoseError error = classify(reader, result.matrixGroup, vtables, result.kind);
        error != GuestPoseError::None) {
        return error;
    }

    std::array<std::uint16_t, native_render::kMaxModelMatrices> slotIndices{};
    if (const GuestPoseError error = read_slot_indices(reader, result.matrixGroup, result.kind,
                                                       slotIndices, result.declaredSlotCount);
        error != GuestPoseError::None) {
        return error;
    }

    const bool cpuPosition = guest_pipeline_positions_are_view_space(result.pipeline);
    const bool cpuNormal = guest_pipeline_normals_are_view_space(result.pipeline);
    // Read for every pipeline, not only the two that transform with it: what the current view is
    // has to be recorded even where it is unused, because a draw matrix carrying a stale camera is
    // only visible next to the camera the title is drawing with now.
    if (!read_matrix(reader, system + SYSTEM_VIEW_MTX, result.viewMatrix)) {
        return GuestPoseError::UnreadableViewMatrix;
    }
    const native_render::Matrix3x4& viewMatrix = result.viewMatrix;

    // What this group loads. A 0xffff slot is not loaded and not empty: it keeps the matrix an
    // earlier group put in that register, which is why nothing is written to `registers` for it.
    for (std::uint32_t slot = 0; slot < result.declaredSlotCount; ++slot) {
        if (slotIndices[slot] == kGuestUnusedMatrixIndex) {
            continue;
        }
        if (slotIndices[slot] >= result.drawMatrixCount) {
            return GuestPoseError::MatrixIndexOutOfRange;
        }
        native_render::Matrix3x4 matrix = viewMatrix;
        if (!cpuPosition &&
            !read_matrix(reader, result.matrixPalette + slotIndices[slot] * MATRIX_BYTES, matrix)) {
            return GuestPoseError::UnreadableMatrix;
        }
        native_render::Matrix3x4 normalMatrix = rotation_of(viewMatrix);
        if (!cpuNormal && !read_normal_matrix(reader,
                                              result.normalMatrixPalette +
                                                  slotIndices[slot] * NORMAL_MATRIX_BYTES,
                                              normalMatrix)) {
            return GuestPoseError::UnreadableNormalMatrix;
        }
        if (!native_render::valid(matrix) || !native_render::valid(normalMatrix)) {
            return GuestPoseError::InvalidMatrix;
        }
        registers.load(slot, matrix, normalMatrix, slotIndices[slot]);
        result.loadedSlotMask |= 1U << slot;
        result.loadedSlotCount += 1;
    }

    // The pose is every register that currently holds a matrix, not only the ones this group wrote,
    // because its geometry is free to name any of them.
    std::uint8_t used = 0;
    for (std::uint32_t slot = 0; slot < kGuestMatrixSlotCount; ++slot) {
        if (!registers.loaded(slot)) {
            continue;
        }
        result.pose.modelViews[used] = registers.position(slot);
        result.normalViews[used] = registers.normal(slot);
        result.drawMatrixIndex[used] = registers.draw_matrix_index(slot);
        result.slotToPoseIndex[slot] = used;
        if ((result.loadedSlotMask & (1U << slot)) == 0) {
            result.inheritedSlotCount += 1;
        }
        ++used;
    }
    if (used == 0) {
        return GuestPoseError::NoUsedMatrices;
    }
    result.pose.count = used;

    out = result;
    return GuestPoseError::None;
}

} // namespace sb::title_adapter
