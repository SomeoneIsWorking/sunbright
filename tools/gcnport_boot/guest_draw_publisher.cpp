// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_draw_publisher.h"

#include "draw_listing.h"
#include "family_color_map.h"

#include <sunbright/native_render/j3d_mesh_vertices.h>
#include <sunbright/native_render/j3d_projection.h>
#include <sunbright/native_render/j3d_tex_coord_generation.h>

#include <sunbright/native_render/semantic_sink.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>

namespace sunbright::gcnport_boot {
namespace {

bool read_through_guest_context(sb::title_adapter::GuestAddress address,
                                std::span<std::uint8_t> destination, void* context) {
    auto* const guest = static_cast<gcnport::GuestContext*>(context);
    return guest->read_memory(address, std::as_writable_bytes(destination));
}

bool read_through_byte_address(sb::native_render::ByteAddress address,
                               std::span<std::uint8_t> destination, void* context) {
    std::uint64_t guestAddress = 0;
    if (!address.guest_value(guestAddress)) {
        return false;
    }
    return read_through_guest_context(static_cast<sb::title_adapter::GuestAddress>(guestAddress),
                                      destination, context);
}

} // namespace

bool GuestDrawPublisher::refuse_non_model(
    const sb::native_render::SemanticDraw& draw,
    std::span<const sb::native_render::DecodedImageView> images, void* context) {
    static_cast<void>(draw);
    static_cast<void>(images);
    static_cast<void>(static_cast<GuestDrawPublisher*>(context)->nonModelDraws_ += 1);
    return false;
}

bool GuestDrawPublisher::accept(const sb::native_render::ModelDraw& draw,
                                const sb::native_render::MeshResourceView& mesh,
                                std::span<const sb::native_render::DecodedImageView> images,
                                void* context) {
    static_cast<void>(draw);
    static_cast<void>(mesh);
    static_cast<void>(images);
    static_cast<void>(static_cast<GuestDrawPublisher*>(context)->acceptedBySink_ += 1);
    return true;
}

void GuestDrawPublisher::diagnose(const sb::native_render::ModelDraw& draw,
                                  const sb::native_render::MeshResourceView& mesh,
                                  std::span<const sb::native_render::DecodedImageView> images) {
    bool named = false;
    if (!sb::native_render::valid(draw)) {
        invalidDraw_ += 1;
        named = true;
    }
    if (!sb::native_render::valid(mesh)) {
        invalidMesh_ += 1;
        named = true;
    }
    if (draw.mesh.resource != mesh.resource || draw.mesh.revision != mesh.revision ||
        draw.mesh.vertexCount != mesh.vertices.size()) {
        mismatchedMesh_ += 1;
        named = true;
    }
    if (!std::ranges::all_of(mesh.vertices, [&](const sb::native_render::MeshVertex& vertex) {
            return vertex.matrixIndex < draw.pose.count;
        })) {
        unindexablePose_ += 1;
        named = true;
    }
    if (!sb::native_render::material_images_match(draw.material, images)) {
        mismatchedImages_ += 1;
        named = true;
    }
    // A rejection nothing here explains means the sink refuses on a ground this does not know
    // about. Counting it separately keeps the total honest instead of silently losing it.
    if (!named) {
        rejectedForNoNamedReason_ += 1;
    }
}

bool GuestDrawPublisher::ensure_sink() {
    if (lease_ || borrowedSink_) {
        return true;
    }
    if (nonModelDraws_ != 0) {
        std::printf("gmse01_boot:   %llu non-model draw(s) were offered to this sink and refused\n",
                    static_cast<unsigned long long>(nonModelDraws_));
    }
    if (sinkFailed_) {
        return false;
    }
    // The process has exactly one semantic sink. When the run renders, the frame bridge holds it
    // for the frame's lifetime and these draws must reach it; claiming a second would fail and
    // would read as "the sink could not be claimed" rather than "someone else is drawing these".
    if (sb::native_render::has_semantic_sink()) {
        borrowedSink_ = true;
        return true;
    }
    const sb::native_render::SemanticSink sink{
        .submit = refuse_non_model, .submitModel = accept, .context = this};
    if (!sb::native_render::claim_semantic_sink(sink, lease_)) {
        sinkFailed_ = true;
        return false;
    }
    return true;
}

GuestDrawPublisher::~GuestDrawPublisher() {
    if (lease_) {
        static_cast<void>(sb::native_render::release_semantic_sink(lease_));
    }
}

std::uint32_t
GuestDrawPublisher::publish(gcnport::GuestContext& guest,
                            const sb::title_adapter::GuestShape& shape, std::uint64_t instance,
                            const sb::native_render::ClassifiedJ3dMaterial& classified,
                            const sb::title_adapter::GuestTexGenBlock& texGen) {
    const sb::native_render::J3dTexCoordGeneration generation =
        sb::title_adapter::build_guest_tex_coord_generation(texGen);
    for (std::uint32_t coordinate = 0; coordinate < generation.count; ++coordinate) {
        const sb::native_render::J3dTexCoordGenerator& generator =
            generation.generators[coordinate];
        coordinateGenerators_[{static_cast<std::uint8_t>(generator.type), generator.source}] += 1;
    }
    if (!ensure_sink()) {
        return 0;
    }
    const sb::title_adapter::GuestMemory memory{read_through_guest_context, &guest};
    std::array<sb::native_render::DecodedImageView, sb::native_render::kMaxClassifiedTextures>
        storage{};
    const std::span<const sb::native_render::DecodedImageView> images =
        sb::native_render::j3d_material_image_views(classified, storage);
    const sb::native_render::Matrix4x4* const projection =
        sb::native_render::current_j3d_projection();

    std::uint32_t published = 0;
    for (std::uint16_t element = 0; element < shape.elementCount; ++element) {
        groups_ += 1;
        const sb::title_adapter::GuestShapeGeometry geometry =
            sb::title_adapter::read_guest_shape_geometry(
                memory, {read_through_byte_address, &guest}, shape, element, system_,
                sb::title_adapter::GuestMatrixGroupVtables{}, registers_, triangles_);
        elementErrors_[geometry.elementError] += 1;
        if (geometry.elementError != sb::title_adapter::GuestShapeError::None) {
            continue;
        }
        meshErrors_[geometry.mesh.error] += 1;
        poseErrors_[geometry.poseError] += 1;
        if (!geometry.complete()) {
            continue;
        }
        if (!sb::native_render::build_j3d_mesh_vertices(triangles_, geometry.pose.slotToPoseIndex,
                                                        vertices_)) {
            unmappedMatrixSlots_ += 1;
            continue;
        }
        composed_ += 1;
        // GX does not sample with the coordinates the vertices carry; it samples with what the
        // material's generators make of them. Applying that here, before the mesh is built, is what
        // keeps the scale and scroll a title puts in its texture matrices out of the renderer.
        sb::native_render::J3dTexCoordGenerationCounts generated{};
        const sb::native_render::J3dTexCoordGenerationResult generation_result =
            sb::native_render::apply_j3d_tex_coord_generation(generation, triangles_, generated);
        coordinateGeneration_[generation_result] += 1;
        if (generation_result != sb::native_render::J3dTexCoordGenerationResult::Success) {
            continue;
        }
        transformedCoordinates_ += generated.transformed;
        authoredCoordinates_ += generated.authored;
        deferredCoordinates_ += generated.deferred;
        // A draw with no projection is not submitted. The identity matrix would be accepted by the
        // sink and would draw the geometry in clip space, which looks like a renderer fault rather
        // than the missing input it is.
        if (projection == nullptr) {
            withoutProjection_ += 1;
            continue;
        }

        const std::uint64_t resource = geometry.element.displayList;
        const std::uint64_t revision = sb::native_render::mesh_revision(vertices_);
        sb::native_render::ModelDraw draw{};
        draw.instance = instance ^ (static_cast<std::uint64_t>(element) << 56U);
        draw.mesh = {resource, revision, static_cast<std::uint32_t>(vertices_.size())};
        draw.pose = geometry.pose.pose;
        draw.projection = *projection;
        draw.material = classified.material;
        draw.fog = classified.fog;
        // A flat family colour binds no texture, so that mode submits no images; the opaque mode
        // keeps the material and its textures and changes only how the result is combined.
        std::span<const sb::native_render::DecodedImageView> submittedImages = images;
        if (mode_ == DrawDiagnosticMode::FamilyMap) {
            draw.material = family_map_material(
                classified.family, sb::native_render::raster_policy(classified.material));
            submittedImages = {};
        } else if (mode_ == DrawDiagnosticMode::Opaque) {
            draw.material = opaque_material(classified.material);
        }
        // The budget numbers the draw, so the listing and a bisection over `--draw-limit` use one
        // numbering. Withheld draws are listed too: the listing is of the frame the title composed,
        // and a bound that cut it in half would otherwise hide the half being looked for.
        const bool withinBudget = budget_ == nullptr || budget_->take();
        if (logFrame_ != 0 && budget_ != nullptr && budget_->frames() == logFrame_) {
            print_draw_listing(logFrame_, budget_->offered(), classified, texGen, images,
                               triangles_, draw, vertices_, resource);
        }
        if (!withinBudget) {
            withheldByBudget_ += 1;
            continue;
        }
        const sb::native_render::MeshResourceView mesh{resource, revision, vertices_};
        if (!sb::native_render::submit_model(draw, mesh, submittedImages)) {
            rejectedBySink_ += 1;
            diagnose(draw, mesh, submittedImages);
            continue;
        }
        submitted_ += 1;
        const sb::native_render::ModelRasterPolicy& policy =
            sb::native_render::raster_policy(classified.material);
        policies_[{.family = classified.family,
                   .blend = policy.blend,
                   .alphaTest = policy.alphaTest,
                   .depthWrite = policy.depthWrite}] += 1;
        verticesSubmitted_ += vertices_.size();
        published += 1;
    }
    return published;
}

bool parse_draw_diagnostic_mode(std::string_view name, DrawDiagnosticMode& mode) {
    if (name == "normal") {
        mode = DrawDiagnosticMode::Normal;
        return true;
    }
    if (name == "family-map") {
        mode = DrawDiagnosticMode::FamilyMap;
        return true;
    }
    if (name == "opaque") {
        mode = DrawDiagnosticMode::Opaque;
        return true;
    }
    return false;
}

const char* draw_diagnostic_mode_name(DrawDiagnosticMode mode) noexcept {
    switch (mode) {
    case DrawDiagnosticMode::Normal:
        return "normal";
    case DrawDiagnosticMode::FamilyMap:
        return "family-map";
    case DrawDiagnosticMode::Opaque:
        return "opaque";
    }
    return "unknown";
}

void GuestDrawPublisher::report() const {
    if (budget_ != nullptr && budget_->bounded()) {
        std::printf("gmse01_boot:   DRAW BUDGET: each frame's first %llu draw(s) were withheld and "
                    "at most %llu after them reached the sink, %llu withheld in total across %llu "
                    "frame(s); this run's image is a slice of each frame, not a rendering result\n",
                    static_cast<unsigned long long>(budget_->skip()),
                    static_cast<unsigned long long>(budget_->limit()),
                    static_cast<unsigned long long>(withheldByBudget_),
                    static_cast<unsigned long long>(budget_->frames()));
    }
    if (mode_ == DrawDiagnosticMode::Opaque) {
        std::printf("gmse01_boot:   OPAQUE MODE: every draw was submitted with blending and the "
                    "alpha test off, so this run's image is not a rendering result\n");
    }
    if (mode_ == DrawDiagnosticMode::FamilyMap) {
        std::printf("gmse01_boot:   FAMILY MAP: every draw was flat-shaded by family, so this run's"
                    " image attributes coverage and is not a rendering result. Legend:");
        for (std::size_t family = 0; family < sb::native_render::kJ3dMaterialFamilyCount;
             ++family) {
            const auto value = static_cast<sb::native_render::J3dMaterialFamily>(family);
            const sb::native_render::Color color = family_map_color(value);
            std::printf(" %s=%02x%02x%02x", sb::native_render::j3d_material_family_name(value),
                        static_cast<unsigned>(color.r * 255.0F + 0.5F),
                        static_cast<unsigned>(color.g * 255.0F + 0.5F),
                        static_cast<unsigned>(color.b * 255.0F + 0.5F));
        }
        std::printf("\n");
    }
    std::printf("gmse01_boot: guest draw publisher: %llu matrix group(s), %llu composed, %llu "
                "submitted, %llu vertex(es)\n",
                static_cast<unsigned long long>(groups_),
                static_cast<unsigned long long>(composed_),
                static_cast<unsigned long long>(submitted_),
                static_cast<unsigned long long>(verticesSubmitted_));
    if (borrowedSink_) {
        // `acceptedBySink_` counts what this object's own counting sink saw, and it never
        // installed one. Saying so is the difference between a renderer that took the draws and a
        // sink that refused every one of them, which print identically as a zero.
        std::printf("gmse01_boot:   another owner in this process holds the semantic sink; the "
                    "submitted count above is what reached it\n");
    }
    std::printf("gmse01_boot:   %llu reached the sink; of those rejected: %llu invalid draw, %llu "
                "invalid mesh, %llu mismatched mesh, %llu unindexable pose, %llu mismatched "
                "images, %llu for no reason this knows\n",
                static_cast<unsigned long long>(acceptedBySink_),
                static_cast<unsigned long long>(invalidDraw_),
                static_cast<unsigned long long>(invalidMesh_),
                static_cast<unsigned long long>(mismatchedMesh_),
                static_cast<unsigned long long>(unindexablePose_),
                static_cast<unsigned long long>(mismatchedImages_),
                static_cast<unsigned long long>(rejectedForNoNamedReason_));
    if (sinkFailed_) {
        std::printf("gmse01_boot:   REFUSES: the semantic sink could not be claimed, so no draw "
                    "was ever offered\n");
    }
    std::printf("gmse01_boot:   %llu rejected by the sink, %llu had no published projection, %llu "
                "named a matrix slot the pose never filled\n",
                static_cast<unsigned long long>(rejectedBySink_),
                static_cast<unsigned long long>(withoutProjection_),
                static_cast<unsigned long long>(unmappedMatrixSlots_));
    std::printf("gmse01_boot:   policy by family (family/blend/alpha-test/depth-write):");
    for (const auto& [key, count] : policies_) {
        std::printf(" %s/%s/%s/%d=%llu", sb::native_render::j3d_material_family_name(key.family),
                    sb::native_render::model_blend_mode_name(key.blend),
                    sb::native_render::model_alpha_test_name(key.alphaTest),
                    static_cast<int>(key.depthWrite), static_cast<unsigned long long>(count));
    }
    std::printf("\n");
    std::printf("gmse01_boot:   element errors:");
    for (const auto& [error, count] : elementErrors_) {
        std::printf(" %s=%llu", sb::title_adapter::guest_shape_error_name(error),
                    static_cast<unsigned long long>(count));
    }
    std::printf(" | mesh errors:");
    for (const auto& [error, count] : meshErrors_) {
        std::printf(" %s=%llu", sb::native_render::j3d_mesh_decode_error_name(error),
                    static_cast<unsigned long long>(count));
    }
    std::printf(" | pose errors:");
    for (const auto& [error, count] : poseErrors_) {
        std::printf(" %s=%llu", sb::title_adapter::guest_pose_error_name(error),
                    static_cast<unsigned long long>(count));
    }
    std::printf("\n");
    std::printf("gmse01_boot:   texture coordinate generation:");
    for (const auto& [result, count] : coordinateGeneration_) {
        std::printf(" %s=%llu", sb::native_render::j3d_tex_coord_generation_result_name(result),
                    static_cast<unsigned long long>(count));
    }
    std::printf(" | coordinates transformed=%llu authored=%llu deferred=%llu",
                static_cast<unsigned long long>(transformedCoordinates_),
                static_cast<unsigned long long>(authoredCoordinates_),
                static_cast<unsigned long long>(deferredCoordinates_));
    std::printf(" | generators by type/source:");
    for (const auto& [generator, count] : coordinateGenerators_) {
        std::printf(" %u/%u=%llu", generator.first, generator.second,
                    static_cast<unsigned long long>(count));
    }
    std::printf("\n");
}

} // namespace sunbright::gcnport_boot
