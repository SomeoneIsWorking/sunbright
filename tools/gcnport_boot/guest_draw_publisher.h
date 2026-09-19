// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <sunbright/native_render/j3d_material_family.h>
#include <sunbright/native_render/j3d_tex_coord_generation.h>
#include <sunbright/native_render/model.h>
#include <sunbright/native_render/semantic_sink.h>
#include <sunbright/title_adapter/guest_j3d_display_list.h>
#include <sunbright/title_adapter/guest_j3d_texgen.h>
#include <sunbright/title_adapter/guest_j3d_texture.h>
#include <sunbright/title_adapter/guest_shape_geometry.h>

#include "frame_draw_budget.h"
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
// What the publisher does to each draw's material before submitting it. Each mode answers one
// question about a rendered frame that the frame itself cannot: `Normal` renders, and the other two
// are deliberately not renderings.
enum class DrawDiagnosticMode : std::uint8_t {
    // The classified material, as the title authored it.
    Normal,
    // A flat colour naming the material family, drawn opaque: which family covers which part of the
    // image. See `family_color_map.h`.
    FamilyMap,
    // The classified material with blending and the alpha test off: whether a defect in the image
    // comes from what a surface computes or from how it is combined with what is behind it. These
    // are different repairs, and a rendered frame cannot tell them apart.
    Opaque,
};

[[nodiscard]] bool parse_draw_diagnostic_mode(std::string_view name, DrawDiagnosticMode& mode);
[[nodiscard]] const char* draw_diagnostic_mode_name(DrawDiagnosticMode mode) noexcept;

class GuestDrawPublisher {
  public:
    // `budget` may be null, which is an unbounded run; it is not owned here. `logFrame` non-zero
    // prints one line per draw of that frame: a draw ordinal found by bounding the frame names a
    // position, and only this names what is at it.
    GuestDrawPublisher(sb::title_adapter::GuestAddress system, DrawDiagnosticMode mode,
                       FrameDrawBudget* budget, std::uint64_t logFrame) noexcept
        : system_(system), mode_(mode), budget_(budget), logFrame_(logFrame) {}

    // Returns how many draws this shape submitted. `shape` must already have been read.
    std::uint32_t publish(gcnport::GuestContext& guest, const sb::title_adapter::GuestShape& shape,
                          std::uint64_t instance,
                          const sb::native_render::ClassifiedJ3dMaterial& classified,
                          const sb::title_adapter::GuestTexGenBlock& texGen,
                          const sb::title_adapter::GuestTextureTable& textureTable,
                          const sb::title_adapter::GuestDisplayListTextures& displayList,
                          const sb::native_render::J3dMaterialState& materialState);

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
    DrawDiagnosticMode mode_ = DrawDiagnosticMode::Normal;
    FrameDrawBudget* budget_ = nullptr;
    std::uint64_t logFrame_ = 0;
    std::uint64_t withheldByBudget_ = 0;
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
    // Which policy each family's draws carry. A rendered frame can show that surfaces are being
    // combined wrongly without saying which combination is at fault; this names the candidates and
    // their sizes, and a family that turns out to use only one blend mode is one this cannot be.
    struct PolicyKey {
        sb::native_render::J3dMaterialFamily family = sb::native_render::J3dMaterialFamily::None;
        sb::native_render::ModelBlendMode blend = sb::native_render::ModelBlendMode::Replace;
        sb::native_render::ModelAlphaTest alphaTest = sb::native_render::ModelAlphaTest::PassAll;
        bool depthWrite = true;
        auto operator<=>(const PolicyKey&) const = default;
    };
    std::map<PolicyKey, std::uint64_t> policies_;
    // How each draw's texture coordinates were generated, or why they could not be. A family that
    // renders wrongly because its coordinates were left authored looks exactly like one that
    // renders wrongly for any other reason, so the refusals are counted by name rather than
    // absorbed into the draw being skipped.
    std::map<sb::native_render::J3dTexCoordGenerationResult, std::uint64_t> coordinateGeneration_;
    // Every generator a draw carried, by the authored type and source numbers. A refusal counted by
    // its reason says a generation could not be applied; this says which authored combination asked
    // for it, which is what an unimplemented one has to be named from.
    std::map<std::pair<std::uint8_t, std::uint8_t>, std::uint64_t> coordinateGenerators_;
    std::uint64_t transformedCoordinates_ = 0;
    std::uint64_t authoredCoordinates_ = 0;
    std::uint64_t deferredCoordinates_ = 0;
    std::map<sb::title_adapter::GuestShapeError, std::uint64_t> elementErrors_;
    std::map<sb::native_render::J3dMeshDecodeError, std::uint64_t> meshErrors_;
    std::map<sb::title_adapter::GuestPoseError, std::uint64_t> poseErrors_;
};

} // namespace sunbright::gcnport_boot
