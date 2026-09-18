#pragma once

#include <sunbright/native_render/j3d_mesh_decode.h>

#include <array>
#include <cstdint>
#include <span>

namespace sb::native_render {

// Turning a J3D material's texture-coordinate generators into the coordinates its draw samples at.
//
// A model's vertices carry authored texture coordinates, and GX never samples with them directly.
// Each of a material's generators names a source and, usually, a matrix, and the matrix is where a
// title puts every scale and scroll: a cloud layer that drifts, a water surface that ripples and a
// detail texture tiled over terrain are all one authored coordinate set and one moving matrix.
// Drawing with the authored coordinates alone magnifies an eight-texel texture across whatever the
// surface covers, which is not a subtle error.
//
// The matrix is supplied, never derived. J3D computes it every frame in `J3DTexMtx::calc` and
// leaves it in `mTotalMtx`; a runtime adapter hands that over. Recomputing the composition here
// would be a second source of truth for a number the title already owns.
//
// This is the last place GX vocabulary appears. The result is written back into the decoded
// vertices, so everything downstream -- the mesh builder, the semantic draw, the renderer -- sees
// ordinary texture coordinates and cannot tell a transformed one from an authored one.

// A coordinate generator's kind, numbered as J3DTexCoordInfo stores it.
enum class J3dTexGenType : std::uint8_t {
    Matrix3x4 = 0,
    Matrix2x4 = 1,
    Bump0 = 2,
    Bump1 = 3,
    Bump2 = 4,
    Bump3 = 5,
    Bump4 = 6,
    Bump5 = 7,
    Bump6 = 8,
    Bump7 = 9,
    SourceRedGreenTexture = 10,
    SourceRedGreenRaster = 11,
};

// What a generator reads, numbered as J3DTexCoordInfo stores it. Only the sources a generator can
// be resolved from per-vertex without renderer state are named; the rest keep their authored
// numbers and are recognised by number.
enum class J3dTexGenSource : std::uint8_t {
    Position = 0,
    Normal = 1,
    Binormal = 2,
    Tangent = 3,
    TextureCoordinate0 = 4,
};

// One generated coordinate. `matrix` is the three rows GX loads, row-major, and is used only when
// `hasMatrix` is set. The console's identity selector is not the identity matrix stored, it is no
// matrix at all, and the two are kept apart so a matrix slot a title left empty cannot be read as
// an identity it never authored.
struct J3dTexCoordGenerator {
    J3dTexGenType type = J3dTexGenType::Matrix2x4;
    std::uint8_t source = static_cast<std::uint8_t>(J3dTexGenSource::TextureCoordinate0);
    bool hasMatrix = false;
    std::array<float, 12> matrix{};
    bool operator==(const J3dTexCoordGenerator&) const = default;
};

struct J3dTexCoordGeneration {
    std::array<J3dTexCoordGenerator, 8> generators{};
    std::uint32_t count = 0;
    bool operator==(const J3dTexCoordGeneration&) const = default;
};

enum class J3dTexCoordGenerationResult : std::uint8_t {
    Success,
    // More generated coordinates than a decoded vertex can carry. The mesh decoder reads four
    // authored sets, and a fifth generated coordinate has nowhere to be written rather than
    // somewhere approximate.
    TooManyCoordinates,
    // A 3x4 generator whose third row produced a divisor of zero for some vertex. Nothing is
    // written: a coordinate set half transformed is worse than one not transformed at all.
    DegenerateProjection,
};

[[nodiscard]] const char*
    j3d_tex_coord_generation_result_name(J3dTexCoordGenerationResult) noexcept;

// What became of each of a material's generators. Every generator lands in exactly one of these,
// so a consumer can tell a material whose coordinates this step produced from one whose it left to
// somebody else -- which is the difference between a texture drawn in the wrong place and one
// drawn by a program that owns its own coordinates.
struct J3dTexCoordGenerationCounts {
    // Rewritten here from an authored set and a matrix.
    std::uint32_t transformed = 0;
    // A matrix-free multiply of the generator's own authored set: already what GX would produce,
    // so the authored values stand unchanged.
    std::uint32_t authored = 0;
    // Bump and colour-sourced generation, and position and normal sources. GX produces these from
    // values this step runs before -- lighting results and post-transform positions -- and the
    // material families that use them resolve their own coordinates in their fragment programs.
    // They are counted rather than approximated, and their authored values are left alone.
    std::uint32_t deferred = 0;
    bool operator==(const J3dTexCoordGenerationCounts&) const = default;
};

// True when this generation leaves every vertex exactly as it is, which is the ordinary case: one
// or two coordinates, each a 2x4 multiply of its own authored set by no matrix. Callers use it to
// skip the rewrite rather than to skip the check.
[[nodiscard]] bool j3d_tex_coord_generation_is_pass_through(const J3dTexCoordGeneration&) noexcept;

// Rewrites each vertex's texture coordinates into what the material's generators produce. All
// generators read the authored sets, so a generator that writes the set another reads does not
// disturb it. Nothing is written when the result is not `Success`.
[[nodiscard]] J3dTexCoordGenerationResult
apply_j3d_tex_coord_generation(const J3dTexCoordGeneration& generation,
                               std::span<J3dDecodedVertex> vertices,
                               J3dTexCoordGenerationCounts& counts) noexcept;

} // namespace sb::native_render
