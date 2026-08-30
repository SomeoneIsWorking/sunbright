#include <sunbright/native_render/model.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <type_traits>

namespace sb::native_render {
namespace {

bool finite(float value) noexcept {
    return std::isfinite(value);
}

bool valid(Color color) noexcept {
    return finite(color.r) && finite(color.g) && finite(color.b) && finite(color.a);
}

Color multiply(Color first, Color second) noexcept {
    return {first.r * second.r, first.g * second.g, first.b * second.b, first.a * second.a};
}

} // namespace

bool valid(const Matrix4x4& matrix) noexcept {
    return std::ranges::all_of(matrix.value, finite);
}

std::uint64_t mesh_revision(std::span<const MeshVertex> vertices) noexcept {
    constexpr std::uint64_t kOffset = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t hash = kOffset;
    const auto append = [&](float value) {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= static_cast<std::uint8_t>(bits >> shift);
            hash *= kPrime;
        }
    };
    for (const MeshVertex& vertex : vertices) {
        append(vertex.position.x);
        append(vertex.position.y);
        append(vertex.position.z);
        append(vertex.uv.x);
        append(vertex.uv.y);
        append(vertex.color.r);
        append(vertex.color.g);
        append(vertex.color.b);
        append(vertex.color.a);
    }
    return hash;
}

Matrix4x4 zero_to_one_depth_projection(Matrix4x4 projection) noexcept {
    for (std::size_t column = 0; column < 4; ++column)
        projection.value[8 + column] += projection.value[12 + column];
    return projection;
}

bool valid(const MeshVertex& vertex) noexcept {
    return finite(vertex.position.x) && finite(vertex.position.y) && finite(vertex.position.z) &&
           finite(vertex.uv.x) && finite(vertex.uv.y) && valid(vertex.color);
}

bool valid(const MeshResourceView& mesh) noexcept {
    return mesh.resource != 0 && !mesh.vertices.empty() && mesh.vertices.size() % 3U == 0U &&
           std::ranges::all_of(mesh.vertices,
                               [](const MeshVertex& vertex) { return valid(vertex); });
}

bool valid(const ModelDraw& draw) noexcept {
    const bool validMaterial = std::visit(
        [](const auto& material) {
            using Material = std::remove_cvref_t<decltype(material)>;
            if constexpr (std::is_same_v<Material, UnlitColorMaterial>)
                return valid(material.baseColor);
            else
                return material.texture.resource != 0 && material.texture.width != 0 &&
                       material.texture.height != 0;
        },
        draw.material);
    return draw.instance != 0 && draw.mesh.resource != 0 && draw.mesh.vertexCount != 0 &&
           draw.mesh.vertexCount % 3U == 0U && valid(draw.modelView) && valid(draw.projection) &&
           validMaterial;
}

ClipVertex transform_vertex(const ModelDraw& draw, const MeshVertex& vertex) noexcept {
    const auto& modelView = draw.modelView.value;
    const float eyeX = modelView[0] * vertex.position.x + modelView[1] * vertex.position.y +
                       modelView[2] * vertex.position.z + modelView[3];
    const float eyeY = modelView[4] * vertex.position.x + modelView[5] * vertex.position.y +
                       modelView[6] * vertex.position.z + modelView[7];
    const float eyeZ = modelView[8] * vertex.position.x + modelView[9] * vertex.position.y +
                       modelView[10] * vertex.position.z + modelView[11];
    const auto& projection = draw.projection.value;
    const Vec4 position{
        projection[0] * eyeX + projection[1] * eyeY + projection[2] * eyeZ + projection[3],
        projection[4] * eyeX + projection[5] * eyeY + projection[6] * eyeZ + projection[7],
        projection[8] * eyeX + projection[9] * eyeY + projection[10] * eyeZ + projection[11],
        projection[12] * eyeX + projection[13] * eyeY + projection[14] * eyeZ + projection[15],
    };
    const Color color = std::visit(
        [&](const auto& material) {
            using Material = std::remove_cvref_t<decltype(material)>;
            if constexpr (std::is_same_v<Material, UnlitColorMaterial>) {
                return material.usesVertexColor ? multiply(material.baseColor, vertex.color)
                                                : material.baseColor;
            } else {
                return material.usesVertexColor ? vertex.color : Color{1.0F, 1.0F, 1.0F, 1.0F};
            }
        },
        draw.material);
    return {position, vertex.uv, color};
}

} // namespace sb::native_render
