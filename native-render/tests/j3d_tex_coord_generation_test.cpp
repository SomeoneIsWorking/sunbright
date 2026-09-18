#include <sunbright/native_render/j3d_tex_coord_generation.h>

#include <cassert>
#include <cmath>
#include <string_view>
#include <vector>

namespace {

using namespace sb::native_render;

constexpr std::uint8_t TEX0 = static_cast<std::uint8_t>(J3dTexGenSource::TextureCoordinate0);
constexpr std::uint8_t TEX1 = TEX0 + 1;

J3dDecodedVertex vertex(float u0, float v0, float u1 = 0.0F, float v1 = 0.0F) {
    J3dDecodedVertex value{};
    value.uv[0][0] = u0;
    value.uv[0][1] = v0;
    value.uv[1][0] = u1;
    value.uv[1][1] = v1;
    return value;
}

bool close(float actual, float expected) {
    return std::fabs(actual - expected) < 1e-5F;
}

// The generation GMSE01's sky material carries: one coordinate, its own authored set, no matrix.
void pass_through_leaves_every_coordinate_alone() {
    J3dTexCoordGeneration generation{};
    generation.count = 1;
    generation.generators[0] = {.type = J3dTexGenType::Matrix2x4, .source = TEX0};
    assert(j3d_tex_coord_generation_is_pass_through(generation));

    std::vector<J3dDecodedVertex> vertices{vertex(0.25F, 0.75F), vertex(-3.0F, 4.5F)};
    const std::vector<J3dDecodedVertex> before = vertices;
    J3dTexCoordGenerationCounts counts{};
    assert(apply_j3d_tex_coord_generation(generation, vertices, counts) ==
           J3dTexCoordGenerationResult::Success);
    assert(vertices == before);
    assert((counts == J3dTexCoordGenerationCounts{.authored = 1}));
}

// A cloud strip: scale and scroll, exactly as read from one of GMSE01's own J3DTexMtx at a frame
// where the layer had drifted.
void a_scale_and_scroll_matrix_moves_the_coordinates() {
    J3dTexCoordGeneration generation{};
    generation.count = 1;
    generation.generators[0] = {
        .type = J3dTexGenType::Matrix2x4,
        .source = TEX0,
        .hasMatrix = true,
        .matrix = {4.0F, 0.0F, 0.0F, -1.5F, 0.0F, 1.0F, 0.0F, 0.475449F, 0.0F, 0.0F, 1.0F, 0.0F}};
    assert(!j3d_tex_coord_generation_is_pass_through(generation));

    std::vector<J3dDecodedVertex> vertices{vertex(0.25F, 0.5F)};
    J3dTexCoordGenerationCounts counts{};
    assert(apply_j3d_tex_coord_generation(generation, vertices, counts) ==
           J3dTexCoordGenerationResult::Success);
    assert((counts == J3dTexCoordGenerationCounts{.transformed = 1}));
    assert(close(vertices[0].uv[0][0], -0.5F));
    assert(close(vertices[0].uv[0][1], 0.975449F));
}

// Two coordinates where the second reads the set the first writes. Generators read authored
// values, so the second must not see the first's result.
void a_generator_does_not_read_what_another_wrote() {
    J3dTexCoordGeneration generation{};
    generation.count = 2;
    generation.generators[0] = {
        .type = J3dTexGenType::Matrix2x4,
        .source = TEX1,
        .hasMatrix = true,
        .matrix = {2.0F, 0.0F, 0.0F, 0.0F, 0.0F, 2.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F}};
    generation.generators[1] = {
        .type = J3dTexGenType::Matrix2x4,
        .source = TEX0,
        .hasMatrix = true,
        .matrix = {0.5F, 0.0F, 0.0F, 0.0F, 0.0F, 0.5F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F}};

    std::vector<J3dDecodedVertex> vertices{vertex(1.0F, 2.0F, 8.0F, 16.0F)};
    J3dTexCoordGenerationCounts counts{};
    assert(apply_j3d_tex_coord_generation(generation, vertices, counts) ==
           J3dTexCoordGenerationResult::Success);
    assert((counts == J3dTexCoordGenerationCounts{.transformed = 2}));
    assert(close(vertices[0].uv[0][0], 16.0F));
    assert(close(vertices[0].uv[0][1], 32.0F));
    assert(close(vertices[0].uv[1][0], 0.5F));
    assert(close(vertices[0].uv[1][1], 1.0F));
}

void a_projected_generator_divides_by_its_third_row() {
    J3dTexCoordGeneration generation{};
    generation.count = 1;
    generation.generators[0] = {
        .type = J3dTexGenType::Matrix3x4,
        .source = TEX0,
        .hasMatrix = true,
        .matrix = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 2.0F, 0.0F}};
    std::vector<J3dDecodedVertex> vertices{vertex(3.0F, 5.0F)};
    J3dTexCoordGenerationCounts counts{};
    assert(apply_j3d_tex_coord_generation(generation, vertices, counts) ==
           J3dTexCoordGenerationResult::Success);
    assert(close(vertices[0].uv[0][0], 1.5F));
    assert(close(vertices[0].uv[0][1], 2.5F));

    generation.generators[0].matrix[10] = 0.0F;
    std::vector<J3dDecodedVertex> degenerate{vertex(3.0F, 5.0F)};
    const std::vector<J3dDecodedVertex> untouched = degenerate;
    assert(apply_j3d_tex_coord_generation(generation, degenerate, counts) ==
           J3dTexCoordGenerationResult::DegenerateProjection);
    assert(degenerate == untouched);
}

// A coordinate this step does not own is left exactly as the vertices carry it, counted, and does
// not stop the coordinates it does own from being produced. The families that use bump and
// colour-sourced generation resolve those coordinates in their own fragment programs.
void a_coordinate_this_step_does_not_own_is_deferred_not_approximated() {
    J3dTexCoordGeneration generation{};
    generation.count = 2;
    generation.generators[0] = {
        .type = J3dTexGenType::Matrix2x4,
        .source = TEX0,
        .hasMatrix = true,
        .matrix = {9.0F, 0.0F, 0.0F, 0.0F, 0.0F, 9.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F}};
    generation.generators[1] = {.type = J3dTexGenType::SourceRedGreenRaster, .source = 19};
    assert(!j3d_tex_coord_generation_is_pass_through(generation));

    std::vector<J3dDecodedVertex> vertices{vertex(1.0F, 2.0F, 3.0F, 4.0F)};
    J3dTexCoordGenerationCounts counts{};
    assert(apply_j3d_tex_coord_generation(generation, vertices, counts) ==
           J3dTexCoordGenerationResult::Success);
    assert((counts == J3dTexCoordGenerationCounts{.transformed = 1, .deferred = 1}));
    assert(close(vertices[0].uv[0][0], 9.0F));
    assert(close(vertices[0].uv[0][1], 18.0F));
    assert(close(vertices[0].uv[1][0], 3.0F));
    assert(close(vertices[0].uv[1][1], 4.0F));

    // A position source is deferred for the same reason: GX generates it from a transformed
    // position that does not exist when a mesh is decoded.
    J3dTexCoordGeneration positioned{};
    positioned.count = 1;
    positioned.generators[0] = {
        .type = J3dTexGenType::Matrix2x4,
        .source = static_cast<std::uint8_t>(J3dTexGenSource::Position),
        .hasMatrix = true,
        .matrix = {5.0F, 0.0F, 0.0F, 0.0F, 0.0F, 5.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F}};
    std::vector<J3dDecodedVertex> unposed{vertex(1.0F, 2.0F)};
    const std::vector<J3dDecodedVertex> before = unposed;
    assert(apply_j3d_tex_coord_generation(positioned, unposed, counts) ==
           J3dTexCoordGenerationResult::Success);
    assert((counts == J3dTexCoordGenerationCounts{.deferred = 1}));
    assert(unposed == before);
}

// More generated coordinates than a decoded vertex carries is a refusal, not a partial write.
void more_coordinates_than_a_vertex_carries_is_refused() {
    J3dTexCoordGeneration generation{};
    generation.count = kJ3dTextureCoordinateSets + 1;
    generation.generators[0] = {
        .type = J3dTexGenType::Matrix2x4,
        .source = TEX0,
        .hasMatrix = true,
        .matrix = {7.0F, 0.0F, 0.0F, 0.0F, 0.0F, 7.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F}};
    std::vector<J3dDecodedVertex> vertices{vertex(1.0F, 2.0F, 3.0F, 4.0F)};
    const std::vector<J3dDecodedVertex> before = vertices;
    J3dTexCoordGenerationCounts counts{};
    assert(apply_j3d_tex_coord_generation(generation, vertices, counts) ==
           J3dTexCoordGenerationResult::TooManyCoordinates);
    assert(vertices == before);
    assert(!j3d_tex_coord_generation_is_pass_through(generation));
}

void every_result_names_itself() {
    const J3dTexCoordGenerationResult results[] = {
        J3dTexCoordGenerationResult::Success,
        J3dTexCoordGenerationResult::TooManyCoordinates,
        J3dTexCoordGenerationResult::DegenerateProjection,
    };
    for (const J3dTexCoordGenerationResult result : results) {
        assert(std::string_view(j3d_tex_coord_generation_result_name(result)) != "unknown");
    }
}

} // namespace

int main() {
    pass_through_leaves_every_coordinate_alone();
    a_scale_and_scroll_matrix_moves_the_coordinates();
    a_generator_does_not_read_what_another_wrote();
    a_projected_generator_divides_by_its_third_row();
    a_coordinate_this_step_does_not_own_is_deferred_not_approximated();
    more_coordinates_than_a_vertex_carries_is_refused();
    every_result_names_itself();
    return 0;
}
