// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include <sunbright/native_render/j3d_material_family.h>
#include <sunbright/native_render/model.h>
#include <sunbright/native_render/semantic_sink.h>
#include <sunbright/title_adapter/guest_shape_geometry.h>

#include "gcnport/guest_context.h"

namespace sunbright::gcnport_boot {

// Builds and submits the `native_render::ModelDraw`s for one classified shape.
//
// This is the end of the guest path: a shape's geometry, the pose it is drawn under, the material
// the classifier resolved, its decoded textures, the stage light and the projection, composed into
// the same structure the decomp runtime submits and handed to the same sink. Everything before it
// measured a part; this is where the parts have to agree with each other or the sink refuses them.
//
// It is separate from the material probe on purpose. Classifying a material and composing a draw
// are different jobs with different failure modes -- a material can classify perfectly and still
// belong to a shape whose pose could not be read -- and keeping the counts apart is what lets a
// refusal be attributed to the right half.
class GuestDrawPublisher {
  public:
    explicit GuestDrawPublisher(sb::title_adapter::GuestAddress system) noexcept
        : system_(system) {}

    // Returns how many draws this shape submitted. `shape` must already have been read.
    std::uint32_t publish(gcnport::GuestContext& guest, const sb::title_adapter::GuestShape& shape,
                          std::uint64_t instance,
                          const sb::native_render::ClassifiedJ3dMaterial& classified);

    void report() const;

    // The sink is held for the whole run rather than claimed per draw, because claiming it is what
    // makes `submit_model` do anything at all, and released here so the lease does not outlive the
    // object the sink's context points at.
    ~GuestDrawPublisher();
    GuestDrawPublisher(const GuestDrawPublisher&) = delete;
    GuestDrawPublisher& operator=(const GuestDrawPublisher&) = delete;
    GuestDrawPublisher(GuestDrawPublisher&&) = delete;
    GuestDrawPublisher& operator=(GuestDrawPublisher&&) = delete;

  private:
    // The sink the publisher installs. It draws nothing and checks nothing: `submit_model` already
    // refuses a draw that is not internally consistent, so a validator here could only repeat
    // checks that have already passed and would never report the other answer. It counts what
    // reached it. Submitting into no sink at all would return false for every draw and read as
    // universal rejection, which is why this exists rather than leaving it unset.
    // A sink must supply both callbacks. This publisher composes models only, so the 2D one refuses
    // and counts: a 2D draw arriving here would mean something else claimed to be publishing
    // through this sink, which is worth seeing rather than dropping.
    static bool refuse_non_model(const sb::native_render::SemanticDraw& draw,
                                 std::span<const sb::native_render::DecodedImageView> images,
                                 void* context);
    static bool accept(const sb::native_render::ModelDraw& draw,
                       const sb::native_render::MeshResourceView& mesh,
                       std::span<const sb::native_render::DecodedImageView> images, void* context);
    bool ensure_sink();

    // Why `submit_model` turned a draw down. Asked only after it has, so each counter names a
    // rejection that actually happened rather than a check that always passes.
    void diagnose(const sb::native_render::ModelDraw& draw,
                  const sb::native_render::MeshResourceView& mesh,
                  std::span<const sb::native_render::DecodedImageView> images);

    sb::title_adapter::GuestAddress system_ = 0;
    sb::title_adapter::GuestMatrixRegisters registers_{};
    std::vector<sb::native_render::J3dDecodedVertex> triangles_;
    std::vector<sb::native_render::MeshVertex> vertices_;

    std::uint64_t groups_ = 0;
    std::uint64_t composed_ = 0;
    std::uint64_t submitted_ = 0;
    std::uint64_t rejectedBySink_ = 0;
    std::uint64_t withoutProjection_ = 0;
    std::uint64_t unmappedMatrixSlots_ = 0;
    std::uint64_t verticesSubmitted_ = 0;
    std::uint64_t acceptedBySink_ = 0;
    std::uint64_t nonModelDraws_ = 0;
    std::uint64_t invalidDraw_ = 0;
    std::uint64_t invalidMesh_ = 0;
    std::uint64_t mismatchedMesh_ = 0;
    std::uint64_t unindexablePose_ = 0;
    std::uint64_t mismatchedImages_ = 0;
    std::uint64_t rejectedForNoNamedReason_ = 0;
    bool sinkFailed_ = false;
    // Set when another owner in this process already holds the one semantic sink -- the frame
    // bridge, when the run was asked to render. The publisher then submits into theirs rather than
    // claiming a second, which would fail and leave every draw unoffered.
    bool borrowedSink_ = false;
    sb::native_render::SemanticSinkLease lease_{};
    std::map<sb::title_adapter::GuestShapeError, std::uint64_t> elementErrors_;
    std::map<sb::native_render::J3dMeshDecodeError, std::uint64_t> meshErrors_;
    std::map<sb::title_adapter::GuestPoseError, std::uint64_t> poseErrors_;
};

} // namespace sunbright::gcnport_boot
