// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <vector>

#include <sunbright/title_adapter/guest_j3d_pose.h>
#include <sunbright/title_adapter/guest_j3d_shape.h>

#include "gcnport/guest_context.h"
#include "gcnport/native_hooks.h"

namespace sunbright::gcnport_boot {

// A native hook that reads the J3DShape the guest is about to draw, through the same
// `title-adapter` code the eventual override will use, and reports what it found.
//
// It does not replace anything: every entry ends in `call_original_once`, so the title draws
// exactly as it did and this measures the reader rather than the renderer. That is the point --
// `title-adapter`'s own test proves the offsets against a synthetic image it wrote itself, which
// cannot tell whether those offsets describe the retail objects GMSE01 actually builds. Only the
// real title can answer that, and only by being read while it runs.
//
// Every matrix group it reads is also decoded, through `native-render`'s own
// `decode_j3d_mesh_element`. Reading the fields only proves the offsets; running the display list
// through the decoder is what proves the vertex layout those fields describe is the one the title's
// geometry was authored against, because a wrong stride or a wrong attribute type produces a
// truncated list or an out-of-range index rather than a plausible mesh.
//
// Each matrix group's pose is read too, and then joined to the geometry: every decoded vertex names
// a GX matrix slot, and the pose says which of those slots holds a matrix by the time the group is
// drawn -- its own, or one an earlier group left there.
// Checking one against the other is the only measurement here that either half could fail on its
// own -- a pose read from the wrong table and a display list decoded at the wrong stride both look
// entirely reasonable until they are asked to agree.
//
// Bounded in the two directions that matter. `max_reports` caps the per-shape detail, because a
// function entered a quarter of a million times cannot print per entry; the histograms below are
// unbounded in coverage and bounded in size, so they describe every entry without growing with the
// run. Both are printed even when they are empty: a probe that reports nothing has not told you
// the title drew nothing, only that it never looked.
class GuestShapeProbe {
  public:
    GuestShapeProbe(sb::title_adapter::GuestAddress system, std::uint64_t max_reports) noexcept
        : system_(system), maxReports_(max_reports) {}

    gcnport::HookResult operator()(gcnport::GuestContext& guest);

    void report() const;

  private:
    // Distinct values are few by construction -- element counts and vertex strides come from a
    // model loader, not from arbitrary data -- but the cap means a corrupt read cannot turn this
    // into unbounded growth, and anything past it is counted rather than dropped.
    static constexpr std::size_t MAX_DISTINCT_VALUES = 32;

    // Enough disagreeing groups to see whether they share a shape, a table size or a pattern, and
    // few enough that a run which disagrees everywhere still finishes.
    static constexpr std::uint64_t MAX_UNPOSED_REPORTS = 8;

    void record(std::map<std::uint32_t, std::uint64_t>& histogram, std::uint64_t& untracked,
                std::uint32_t value);

    // Prints one matrix group whose geometry named a slot its pose never loaded, with the slot
    // table as it sits in guest memory beside the slots the display list actually used. A count of
    // disagreements says only that there are some; this says which, which is what tells a
    // misread field apart from a title that really does leave a matrix slot stale.
    void report_unposed_group(gcnport::GuestContext& guest,
                              const sb::title_adapter::GuestShapePose& pose, std::uint16_t element,
                              std::uint32_t slotsUsed);

    sb::title_adapter::GuestAddress system_ = 0;
    std::uint64_t maxReports_ = 0;

    std::uint64_t entries_ = 0;
    std::uint64_t reports_ = 0;
    std::uint64_t shapesRead_ = 0;
    std::uint64_t elementsRead_ = 0;
    std::uint64_t displayListBytes_ = 0;
    std::uint64_t trianglesDecoded_ = 0;
    std::uint64_t posesRead_ = 0;
    std::uint64_t matricesPosed_ = 0;
    std::uint64_t slotsChecked_ = 0;
    std::uint64_t slotsUnposed_ = 0;
    std::uint64_t groupsWithHoles_ = 0;
    std::uint64_t groupsWithUnposedSlots_ = 0;
    std::uint64_t slotsInherited_ = 0;
    std::uint64_t slotsInheritedAcrossShapes_ = 0;
    std::uint64_t unposedReports_ = 0;
    std::uint32_t smallestElement_ = 0;
    std::uint32_t largestElement_ = 0;

    // Reused across every element so a quarter of a million decodes do not each allocate. The
    // decoder clears and refills it.
    std::vector<sb::native_render::J3dDecodedVertex> triangles_;

    std::map<sb::title_adapter::GuestShapeError, std::uint64_t> shapeErrors_;
    std::map<sb::title_adapter::GuestShapeError, std::uint64_t> elementErrors_;
    std::map<sb::native_render::J3dMeshDecodeError, std::uint64_t> decodeErrors_;
    std::map<sb::title_adapter::GuestPoseError, std::uint64_t> poseErrors_;
    std::map<sb::title_adapter::GuestMatrixGroupKind, std::uint64_t> poseKinds_;
    std::map<sb::title_adapter::GuestSkinningPipeline, std::uint64_t> posePipelines_;
    std::map<std::uint32_t, std::uint64_t> poseSizes_;
    std::uint64_t poseSizesUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> declaredSlotCounts_;
    std::uint64_t declaredSlotCountsUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> unposedSlots_;
    std::uint64_t unposedSlotsUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> inheritedSlotCounts_;
    std::uint64_t inheritedSlotCountsUntracked_ = 0;

    // GX's matrix registers, carried in draw order across every matrix group and every shape --
    // which is what they are. Resetting it per shape would report exactly the disagreement it is
    // here to measure.
    sb::title_adapter::GuestMatrixRegisters registers_;

    // Which shape last loaded each matrix register. Once every register has been filled once, "the
    // pose holds this slot" stops discriminating -- it is true of almost every group. This is the
    // question that still has two answers: a vertex drawn through a register some *other* shape
    // filled is drawn with a matrix from another model's palette, which is what would be happening
    // if the inheritance reading were wrong.
    std::array<sb::title_adapter::GuestAddress, sb::title_adapter::kGuestMatrixSlotCount>
        slotOwner_{};
    std::map<std::uint32_t, std::uint64_t> drawMatrixCounts_;
    std::uint64_t drawMatrixCountsUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> elementCounts_;
    std::uint64_t elementCountsUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> vertexSizes_;
    std::uint64_t vertexSizesUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> descriptorCounts_;
    std::uint64_t descriptorCountsUntracked_ = 0;
};

} // namespace sunbright::gcnport_boot
