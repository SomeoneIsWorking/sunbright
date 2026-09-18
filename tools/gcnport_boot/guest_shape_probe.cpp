// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_shape_probe.h"

#include <array>
#include <cstdio>
#include <limits>
#include <span>

namespace sunbright::gcnport_boot {
namespace {

// gcnport hands the hook a GuestContext; title-adapter wants a plain reader. This is the whole
// adaptation: a `bool` answer either way, so a range the runtime refuses stays refused rather than
// becoming zeroes that read like an empty shape.
bool read_through_guest_context(sb::title_adapter::GuestAddress address,
                                std::span<std::uint8_t> destination, void* context) {
    auto* const guest = static_cast<gcnport::GuestContext*>(context);
    return guest->read_memory(address, std::as_writable_bytes(destination));
}

// The decoder reads through ByteAddress rather than a raw guest address, so it needs its own thin
// adapter onto the same GuestContext. Both end at `guest->read_memory`, so a range the runtime
// refuses is refused identically whichever side asks.
bool read_through_byte_address(sb::native_render::ByteAddress address,
                               std::span<std::uint8_t> destination, void* context) {
    std::uint64_t guestAddress = 0;
    if (!address.guest_value(guestAddress) ||
        guestAddress > std::numeric_limits<sb::title_adapter::GuestAddress>::max()) {
        return false;
    }
    auto* const guest = static_cast<gcnport::GuestContext*>(context);
    return guest->read_memory(static_cast<sb::title_adapter::GuestAddress>(guestAddress),
                              std::as_writable_bytes(destination));
}

void print_histogram(const char* label, const std::map<std::uint32_t, std::uint64_t>& histogram,
                     std::uint64_t untracked) {
    if (histogram.empty()) {
        std::printf("gmse01_boot:   %s: none recorded\n", label);
        return;
    }
    std::printf("gmse01_boot:   %s:", label);
    for (const auto& [value, count] : histogram) {
        std::printf(" %u=%llu", value, static_cast<unsigned long long>(count));
    }
    if (untracked != 0) {
        std::printf(" (+%llu past the tracked distinct values -- truncated, not complete)",
                    static_cast<unsigned long long>(untracked));
    }
    std::printf("\n");
}

} // namespace

void GuestShapeProbe::record(std::map<std::uint32_t, std::uint64_t>& histogram,
                             std::uint64_t& untracked, std::uint32_t value) {
    if (histogram.size() < MAX_DISTINCT_VALUES || histogram.contains(value)) {
        histogram[value] += 1;
        return;
    }
    untracked += 1;
}

void GuestShapeProbe::report_unposed_group(gcnport::GuestContext& guest,
                                           const sb::title_adapter::GuestShapePose& pose,
                                           std::uint16_t element, std::uint32_t slotsUsed) {
    if (unposedReports_ >= MAX_UNPOSED_REPORTS) {
        return;
    }
    unposedReports_ += 1;

    std::printf("gmse01_boot: unposed group 0x%08x element=%u kind=%s declared=%u loaded=%u "
                "table=%u\n",
                pose.matrixGroup, element,
                sb::title_adapter::guest_matrix_group_kind_name(pose.kind), pose.declaredSlotCount,
                pose.pose.count, pose.drawMatrixCount);

    std::array<std::uint8_t, 32> raw{};
    if (guest.read_memory(pose.matrixGroup, std::as_writable_bytes(std::span(raw)))) {
        std::printf("gmse01_boot:   object:");
        for (const std::uint8_t value : raw) {
            std::printf(" %02x", value);
        }
        std::printf("\n");
    }
    std::printf("gmse01_boot:   loaded slot->draw matrix:");
    for (std::uint32_t slot = 0; slot < sb::title_adapter::kGuestMatrixSlotCount; ++slot) {
        const std::uint8_t poseIndex = pose.slotToPoseIndex[slot];
        const bool used = (slotsUsed & (1U << slot)) != 0;
        if (poseIndex == 0xffU) {
            std::printf(" %u=%s", slot, used ? "SKIPPED-BUT-DRAWN" : "skipped");
            continue;
        }
        std::printf(" %u=%u%s", slot, pose.drawMatrixIndex[poseIndex], used ? "*" : "");
    }
    std::printf("\n");
}

gcnport::HookResult GuestShapeProbe::operator()(gcnport::GuestContext& guest) {
    entries_ += 1;

    // J3DShape::draw is a const member function, so the shape is `this` in r3 -- confirmed by its
    // own first instructions at 0x802e0390, which read 0x28(r3) (mGDCommands) and 8(r31) (mFlags).
    constexpr std::size_t THIS_REGISTER = 3;
    const auto shapeAddress =
        static_cast<sb::title_adapter::GuestAddress>(guest.general_register(THIS_REGISTER));

    const sb::title_adapter::GuestMemory memory{read_through_guest_context, &guest};
    sb::title_adapter::GuestShape shape{};
    const sb::title_adapter::GuestShapeError error =
        read_guest_shape(memory, shapeAddress, system_, shape);
    shapeErrors_[error] += 1;
    if (error != sb::title_adapter::GuestShapeError::None) {
        return gcnport::HookResult::call_original_once();
    }

    shapesRead_ += 1;
    record(elementCounts_, elementCountsUntracked_, shape.elementCount);
    record(vertexSizes_, vertexSizesUntracked_, shape.layout.vertexSize);
    record(descriptorCounts_, descriptorCountsUntracked_, shape.descriptorCount);

    const bool reporting = reports_ < maxReports_;
    if (reporting) {
        reports_ += 1;
        std::printf("gmse01_boot: shape 0x%08x index=%u elements=%u flags=0x%08x nbt=%d "
                    "vtx=%u nrm=%u col=%u stride=%u descriptors=%u formats=%u\n",
                    shape.address, shape.index, shape.elementCount, shape.flags,
                    shape.normalBinormalTangent ? 1 : 0, shape.vertexCount, shape.normalCount,
                    shape.colorCount, shape.layout.vertexSize, shape.descriptorCount,
                    shape.attributeFormatCount);
        std::printf("gmse01_boot:   arrays pos=0x%08x nrm=0x%08x col=0x%08x tex0=0x%08x\n",
                    shape.positions, shape.normals, shape.colors, shape.textureCoordinates[0]);
    }

    for (std::uint16_t element = 0; element < shape.elementCount; ++element) {
        sb::title_adapter::GuestShapeGeometry geometry =
            sb::title_adapter::read_guest_shape_geometry(
                memory, {read_through_byte_address, &guest}, shape, element, system_,
                sb::title_adapter::GuestMatrixGroupVtables{}, registers_, triangles_);
        const sb::title_adapter::GuestShapeElement& group = geometry.element;
        elementErrors_[geometry.elementError] += 1;
        if (geometry.elementError != sb::title_adapter::GuestShapeError::None) {
            continue;
        }
        elementsRead_ += 1;
        displayListBytes_ += group.displayListSize;

        const sb::native_render::J3dMeshDecodeResult& decoded = geometry.mesh;
        decodeErrors_[decoded.error] += 1;
        const auto vertices = static_cast<std::uint32_t>(triangles_.size());
        if (decoded.error == sb::native_render::J3dMeshDecodeError::None) {
            trianglesDecoded_ += vertices / 3;
            if (smallestElement_ == 0 || vertices < smallestElement_) {
                smallestElement_ = vertices;
            }
            if (vertices > largestElement_) {
                largestElement_ = vertices;
            }
        }
        const sb::title_adapter::GuestShapePose& pose = geometry.pose;
        poseErrors_[geometry.poseError] += 1;
        const bool posed = geometry.poseError == sb::title_adapter::GuestPoseError::None;
        if (posed) {
            posesRead_ += 1;
            matricesPosed_ += pose.pose.count;
            poseKinds_[pose.kind] += 1;
            posePipelines_[pose.pipeline] += 1;
            record(poseSizes_, poseSizesUntracked_, pose.pose.count);
            record(declaredSlotCounts_, declaredSlotCountsUntracked_, pose.declaredSlotCount);
            record(inheritedSlotCounts_, inheritedSlotCountsUntracked_, pose.inheritedSlotCount);
            if (pose.declaredSlotCount != pose.loadedSlotCount) {
                groupsWithHoles_ += 1;
            }
            record(drawMatrixCounts_, drawMatrixCountsUntracked_, pose.drawMatrixCount);
        }

        // The join. A vertex names a GX matrix register; the pose says which registers this group
        // loaded. Counting the ones it did not is what would catch either half being wrong, and the
        // denominator beside it is what makes a zero mean "checked and agreed" rather than "never
        // looked".
        std::uint32_t unposed = 0;
        std::uint32_t slotsUsed = 0;
        if (posed && decoded.error == sb::native_render::J3dMeshDecodeError::None) {
            for (const sb::native_render::J3dDecodedVertex& vertex : triangles_) {
                slotsChecked_ += 1;
                const std::uint32_t slot = vertex.positionMatrixSlot;
                if (slot < sb::title_adapter::kGuestMatrixSlotCount) {
                    slotsUsed |= 1U << slot;
                    if ((pose.loadedSlotMask & (1U << slot)) == 0) {
                        slotsInherited_ += 1;
                        if (slotOwner_[slot] != shape.address) {
                            slotsInheritedAcrossShapes_ += 1;
                        }
                    }
                }
                if (slot >= sb::title_adapter::kGuestMatrixSlotCount ||
                    pose.slotToPoseIndex[slot] == 0xffU) {
                    unposed += 1;
                    record(unposedSlots_, unposedSlotsUntracked_, slot);
                }
            }
            slotsUnposed_ += unposed;
            if (unposed != 0) {
                groupsWithUnposedSlots_ += 1;
                report_unposed_group(guest, pose, element, slotsUsed);
            }
        }

        for (std::uint32_t slot = 0; slot < sb::title_adapter::kGuestMatrixSlotCount; ++slot) {
            if ((pose.loadedSlotMask & (1U << slot)) != 0) {
                slotOwner_[slot] = shape.address;
            }
        }

        if (reporting) {
            std::printf("gmse01_boot:   element %u draw=0x%08x list=0x%08x size=%u decode=%s "
                        "vertices=%u\n",
                        element, group.draw, group.displayList, group.displayListSize,
                        sb::native_render::j3d_mesh_decode_error_name(decoded.error), vertices);
            if (decoded.error != sb::native_render::J3dMeshDecodeError::None) {
                std::printf("gmse01_boot:     stopped at display-list offset %u, opcode 0x%02x\n",
                            decoded.displayListOffset, decoded.opcode);
            }
            std::printf("gmse01_boot:     pose=%s",
                        sb::title_adapter::guest_pose_error_name(geometry.poseError));
            if (posed) {
                std::printf(" %s/%s matrices=%u of %u view=%u palette=0x%08x unposed=%u",
                            sb::title_adapter::guest_matrix_group_kind_name(pose.kind),
                            sb::title_adapter::guest_skinning_pipeline_name(pose.pipeline),
                            pose.pose.count, pose.drawMatrixCount, pose.viewNumber,
                            pose.matrixPalette, unposed);
                std::printf(" loaded=%u inherited=%u", pose.loadedSlotCount,
                            pose.inheritedSlotCount);
            }
            std::printf("\n");
        }
    }

    return gcnport::HookResult::call_original_once();
}

void GuestShapeProbe::report() const {
    std::printf("gmse01_boot: shape probe entered %llu time(s), read %llu shape(s) and %llu "
                "matrix group(s), %llu display-list byte(s)\n",
                static_cast<unsigned long long>(entries_),
                static_cast<unsigned long long>(shapesRead_),
                static_cast<unsigned long long>(elementsRead_),
                static_cast<unsigned long long>(displayListBytes_));
    if (entries_ == 0) {
        std::printf("gmse01_boot:   installed, never dispatched -- no shape was read and nothing "
                    "below was measured\n");
        return;
    }

    std::printf("gmse01_boot:   shape results:");
    for (const auto& [error, count] : shapeErrors_) {
        std::printf(" %s=%llu", sb::title_adapter::guest_shape_error_name(error),
                    static_cast<unsigned long long>(count));
    }
    std::printf("\n");

    std::printf("gmse01_boot:   matrix-group results:");
    if (elementErrors_.empty()) {
        std::printf(" none attempted");
    }
    for (const auto& [error, count] : elementErrors_) {
        std::printf(" %s=%llu", sb::title_adapter::guest_shape_error_name(error),
                    static_cast<unsigned long long>(count));
    }
    std::printf("\n");

    std::printf("gmse01_boot:   decode results:");
    if (decodeErrors_.empty()) {
        std::printf(" none attempted");
    }
    for (const auto& [error, count] : decodeErrors_) {
        std::printf(" %s=%llu", sb::native_render::j3d_mesh_decode_error_name(error),
                    static_cast<unsigned long long>(count));
    }
    std::printf("\n");
    std::printf("gmse01_boot:   %llu triangle(s) decoded; smallest matrix group %u vertices, "
                "largest %u\n",
                static_cast<unsigned long long>(trianglesDecoded_), smallestElement_,
                largestElement_);

    std::printf("gmse01_boot:   pose results:");
    if (poseErrors_.empty()) {
        std::printf(" none attempted");
    }
    for (const auto& [error, count] : poseErrors_) {
        std::printf(" %s=%llu", sb::title_adapter::guest_pose_error_name(error),
                    static_cast<unsigned long long>(count));
    }
    std::printf("\n");

    std::printf("gmse01_boot:   matrix-group kinds:");
    if (poseKinds_.empty()) {
        std::printf(" none read");
    }
    for (const auto& [kind, count] : poseKinds_) {
        std::printf(" %s=%llu", sb::title_adapter::guest_matrix_group_kind_name(kind),
                    static_cast<unsigned long long>(count));
    }
    std::printf("\n");

    std::printf("gmse01_boot:   skinning pipelines:");
    if (posePipelines_.empty()) {
        std::printf(" none read");
    }
    for (const auto& [pipeline, count] : posePipelines_) {
        std::printf(" %s=%llu", sb::title_adapter::guest_skinning_pipeline_name(pipeline),
                    static_cast<unsigned long long>(count));
    }
    std::printf("\n");

    std::printf("gmse01_boot:   %llu pose(s) read holding %llu matrix(es); %llu of %llu decoded "
                "vertex matrix slot(s) named a slot the pose never loaded\n",
                static_cast<unsigned long long>(posesRead_),
                static_cast<unsigned long long>(matricesPosed_),
                static_cast<unsigned long long>(slotsUnposed_),
                static_cast<unsigned long long>(slotsChecked_));

    std::printf("gmse01_boot:   %llu matrix group(s) left a declared slot to the group before "
                "them; %llu "
                "matrix group(s) drew at least one vertex through an unloaded slot\n",
                static_cast<unsigned long long>(groupsWithHoles_),
                static_cast<unsigned long long>(groupsWithUnposedSlots_));

    std::printf("gmse01_boot:   %llu of those vertex slot(s) were drawn through a register this "
                "matrix group did not load; %llu of those came from another shape\n",
                static_cast<unsigned long long>(slotsInherited_),
                static_cast<unsigned long long>(slotsInheritedAcrossShapes_));

    print_histogram("matrices per pose", poseSizes_, poseSizesUntracked_);
    print_histogram("slots inherited per pose", inheritedSlotCounts_,
                    inheritedSlotCountsUntracked_);
    print_histogram("slots declared per pose", declaredSlotCounts_, declaredSlotCountsUntracked_);
    print_histogram("unloaded slot named by a vertex", unposedSlots_, unposedSlotsUntracked_);
    print_histogram("model draw-matrix table size", drawMatrixCounts_, drawMatrixCountsUntracked_);
    print_histogram("matrix groups per shape", elementCounts_, elementCountsUntracked_);
    print_histogram("vertex stride (bytes)", vertexSizes_, vertexSizesUntracked_);
    print_histogram("vertex descriptors per shape", descriptorCounts_, descriptorCountsUntracked_);
}

} // namespace sunbright::gcnport_boot
