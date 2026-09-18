#include <sunbright/native_render/j3d_tex_coord_generation.h>

#include <cmath>

namespace sb::native_render {
namespace {

constexpr std::uint8_t FIRST_TEXTURE_COORDINATE_SOURCE =
    static_cast<std::uint8_t>(J3dTexGenSource::TextureCoordinate0);

// The authored set a source names, or `kJ3dTextureCoordinateSets` when it names something else.
std::uint32_t authored_set(std::uint8_t source) noexcept {
    if (source < FIRST_TEXTURE_COORDINATE_SOURCE) {
        return kJ3dTextureCoordinateSets;
    }
    const std::uint32_t set = static_cast<std::uint32_t>(source - FIRST_TEXTURE_COORDINATE_SOURCE);
    return set < kJ3dTextureCoordinateSets ? set : kJ3dTextureCoordinateSets;
}

// Whether this step produces the coordinate at all. Bump and colour-sourced generation, and
// position and normal sources, are produced by GX from values that do not exist yet when a mesh is
// decoded; the families that use them carry their own fragment programs.
bool is_transformable(const J3dTexCoordGenerator& generator) noexcept {
    if (generator.type != J3dTexGenType::Matrix2x4 && generator.type != J3dTexGenType::Matrix3x4) {
        return false;
    }
    return authored_set(generator.source) < kJ3dTextureCoordinateSets;
}

// Whether the generator produces exactly the values already in the vertex: its own authored set,
// multiplied by no matrix. The console's identity selector is not a matrix, so this is the ordinary
// untransformed case rather than a transform that happens to be the identity.
bool is_authored(const J3dTexCoordGenerator& generator, std::uint32_t coordinate) noexcept {
    return is_transformable(generator) && !generator.hasMatrix &&
           generator.type == J3dTexGenType::Matrix2x4 &&
           authored_set(generator.source) == coordinate;
}

float row_dot(const std::array<float, 12>& matrix, std::size_t row, float u, float v) noexcept {
    const std::size_t base = row * 4U;
    // An authored texture coordinate enters the transform as (u, v, 1, 1): the third component is
    // the one J3D's SRT translation multiplies, and the fourth is GX's constant.
    return (matrix[base] * u) + (matrix[base + 1U] * v) + matrix[base + 2U] + matrix[base + 3U];
}

} // namespace

const char* j3d_tex_coord_generation_result_name(J3dTexCoordGenerationResult result) noexcept {
    switch (result) {
    case J3dTexCoordGenerationResult::Success:
        return "success";
    case J3dTexCoordGenerationResult::TooManyCoordinates:
        return "more generated coordinates than a vertex carries";
    case J3dTexCoordGenerationResult::DegenerateProjection:
        return "degenerate 3x4 coordinate projection";
    }
    return "unknown";
}

bool j3d_tex_coord_generation_is_pass_through(const J3dTexCoordGeneration& generation) noexcept {
    if (generation.count > kJ3dTextureCoordinateSets) {
        return false;
    }
    for (std::uint32_t coordinate = 0; coordinate < generation.count; ++coordinate) {
        if (!is_authored(generation.generators[coordinate], coordinate)) {
            return false;
        }
    }
    return true;
}

J3dTexCoordGenerationResult
apply_j3d_tex_coord_generation(const J3dTexCoordGeneration& generation,
                               std::span<J3dDecodedVertex> vertices,
                               J3dTexCoordGenerationCounts& counts) noexcept {
    if (generation.count > kJ3dTextureCoordinateSets) {
        return J3dTexCoordGenerationResult::TooManyCoordinates;
    }
    J3dTexCoordGenerationCounts tally{};
    for (std::uint32_t coordinate = 0; coordinate < generation.count; ++coordinate) {
        const J3dTexCoordGenerator& generator = generation.generators[coordinate];
        if (!is_transformable(generator)) {
            tally.deferred += 1;
        } else if (is_authored(generator, coordinate)) {
            tally.authored += 1;
        } else {
            tally.transformed += 1;
        }
    }
    if (tally.transformed == 0) {
        counts = tally;
        return J3dTexCoordGenerationResult::Success;
    }
    for (J3dDecodedVertex& vertex : vertices) {
        // A copy, because a generator may read the set another writes, and because `vertex.uv` is
        // a C array: deduced by value it would be a pointer into the vertex being rewritten.
        std::array<std::array<float, 2>, kJ3dTextureCoordinateSets> authored{};
        for (std::uint32_t set = 0; set < kJ3dTextureCoordinateSets; ++set) {
            authored[set] = {vertex.uv[set][0], vertex.uv[set][1]};
        }
        for (std::uint32_t coordinate = 0; coordinate < generation.count; ++coordinate) {
            const J3dTexCoordGenerator& generator = generation.generators[coordinate];
            if (!is_transformable(generator) || is_authored(generator, coordinate)) {
                continue;
            }
            const std::uint32_t set = authored_set(generator.source);
            const float u = authored[set][0];
            const float v = authored[set][1];
            float s = row_dot(generator.matrix, 0, u, v);
            float t = row_dot(generator.matrix, 1, u, v);
            if (generator.type == J3dTexGenType::Matrix3x4) {
                const float q = row_dot(generator.matrix, 2, u, v);
                if (q == 0.0F || !std::isfinite(q)) {
                    return J3dTexCoordGenerationResult::DegenerateProjection;
                }
                s /= q;
                t /= q;
            }
            vertex.uv[coordinate][0] = s;
            vertex.uv[coordinate][1] = t;
        }
    }
    counts = tally;
    return J3dTexCoordGenerationResult::Success;
}

} // namespace sb::native_render
