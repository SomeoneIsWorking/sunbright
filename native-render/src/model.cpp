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

bool valid(Vec3 vector) noexcept {
    return finite(vector.x) && finite(vector.y) && finite(vector.z);
}

bool valid(const PointLight& light) noexcept {
    return valid(light.position) && valid(light.color) && valid(light.distanceAttenuation) &&
           light.distanceAttenuation.x >= 0.0F && light.distanceAttenuation.y >= 0.0F &&
           light.distanceAttenuation.z >= 0.0F;
}

bool valid(const ModelLightingContext& lighting) noexcept {
    return valid(lighting.ambientColor) &&
           lighting.pointLightCount <= lighting.pointLights.size() &&
           std::ranges::all_of(lighting.pointLights.begin(),
                               lighting.pointLights.begin() + lighting.pointLightCount,
                               [](const PointLight& light) { return valid(light); });
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
        append(vertex.normal.x);
        append(vertex.normal.y);
        append(vertex.normal.z);
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
           finite(vertex.uv.x) && finite(vertex.uv.y) && valid(vertex.color) &&
           valid(vertex.normal);
}

bool valid(const MeshResourceView& mesh) noexcept {
    return mesh.resource != 0 && !mesh.vertices.empty() && mesh.vertices.size() % 3U == 0U &&
           std::ranges::all_of(mesh.vertices,
                               [](const MeshVertex& vertex) { return valid(vertex); });
}

bool valid(const ModelRasterPolicy& raster) noexcept {
    return raster.cull <= ModelCullMode::All && raster.depthCompare <= ModelDepthCompare::Always &&
           raster.alphaTest <= ModelAlphaTest::GreaterOrEqualHalf &&
           raster.blend <= ModelBlendMode::SourceAlpha;
}

const ModelRasterPolicy& raster_policy(const ModelMaterial& material) noexcept {
    return std::visit([](const auto& value) -> const ModelRasterPolicy& { return value.raster; },
                      material);
}

const PictureTexture* material_texture(const ModelMaterial& material) noexcept {
    return std::visit(
        [](const auto& value) -> const PictureTexture* {
            using Material = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<Material, UnlitTexturedMaterial> ||
                          std::is_same_v<Material, LitTexturedMaterial>) {
                return &value.texture;
            } else {
                return nullptr;
            }
        },
        material);
}

bool valid(const ModelDraw& draw) noexcept {
    const bool validMaterial = std::visit(
        [](const auto& material) {
            using Material = std::remove_cvref_t<decltype(material)>;
            if constexpr (std::is_same_v<Material, UnlitColorMaterial>) {
                return valid(material.baseColor);
            } else if constexpr (std::is_same_v<Material, UnlitTexturedMaterial>) {
                return material.texture.resource != 0 && material.texture.width != 0 &&
                       material.texture.height != 0;
            } else {
                return material.texture.resource != 0 && material.texture.width != 0 &&
                       material.texture.height != 0 && valid(material.baseColor) &&
                       valid(material.ambientColor) && valid(material.lighting) &&
                       finite(material.litColorWeight) && material.litColorWeight >= 0.0F &&
                       material.litColorWeight <= 1.0F;
            }
        },
        draw.material);
    return draw.instance != 0 && draw.mesh.resource != 0 && draw.mesh.vertexCount != 0 &&
           draw.mesh.vertexCount % 3U == 0U && valid(draw.modelView) && valid(draw.projection) &&
           validMaterial && valid(raster_policy(draw.material));
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
            } else if constexpr (std::is_same_v<Material, UnlitTexturedMaterial>) {
                return material.usesVertexColor ? vertex.color : Color{1.0F, 1.0F, 1.0F, 1.0F};
            } else {
                const Color rgbSource = material.usesVertexRgb ? vertex.color : material.baseColor;
                const float alphaSource =
                    material.usesVertexAlpha ? vertex.color.a : material.baseColor.a;
                const float a = modelView[0];
                const float b = modelView[1];
                const float c = modelView[2];
                const float d = modelView[4];
                const float e = modelView[5];
                const float f = modelView[6];
                const float g = modelView[8];
                const float h = modelView[9];
                const float i = modelView[10];
                const float determinant =
                    a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
                Vec3 normal{};
                if (std::fabs(determinant) > 1.0e-12F) {
                    const float inverse = 1.0F / determinant;
                    normal = {
                        ((e * i - f * h) * vertex.normal.x + (f * g - d * i) * vertex.normal.y +
                         (d * h - e * g) * vertex.normal.z) *
                            inverse,
                        ((c * h - b * i) * vertex.normal.x + (a * i - c * g) * vertex.normal.y +
                         (b * g - a * h) * vertex.normal.z) *
                            inverse,
                        ((b * f - c * e) * vertex.normal.x + (c * d - a * f) * vertex.normal.y +
                         (a * e - b * d) * vertex.normal.z) *
                            inverse,
                    };
                }
                const float normalLength =
                    std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
                if (normalLength > 0.0F) {
                    normal.x /= normalLength;
                    normal.y /= normalLength;
                    normal.z /= normalLength;
                }

                Color illumination = material.ambientColor;
                for (std::uint8_t index = 0; index < material.lighting.pointLightCount; ++index) {
                    const PointLight& light = material.lighting.pointLights[index];
                    Vec3 direction{light.position.x - eyeX, light.position.y - eyeY,
                                   light.position.z - eyeZ};
                    const float distance =
                        std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                  direction.z * direction.z);
                    if (distance <= 0.0F)
                        continue;
                    direction.x /= distance;
                    direction.y /= distance;
                    direction.z /= distance;
                    const float diffuse =
                        std::max(0.0F, normal.x * direction.x + normal.y * direction.y +
                                           normal.z * direction.z);
                    const Vec3 attenuation = light.distanceAttenuation;
                    const float denominator = attenuation.x + attenuation.y * distance +
                                              attenuation.z * distance * distance;
                    if (denominator <= 0.0F)
                        continue;
                    const float contribution = diffuse / denominator;
                    illumination.r += light.color.r * contribution;
                    illumination.g += light.color.g * contribution;
                    illumination.b += light.color.b * contribution;
                }
                // J3D/GX clamps the accumulated ambient-plus-light result before applying the
                // material or vertex colour. Clamping only the final product over-brightens dark
                // materials when several lights saturate the accumulator.
                illumination.r = std::clamp(illumination.r, 0.0F, 1.0F);
                illumination.g = std::clamp(illumination.g, 0.0F, 1.0F);
                illumination.b = std::clamp(illumination.b, 0.0F, 1.0F);
                const float weight = material.litColorWeight;
                return Color{
                    std::lerp(1.0F, std::clamp(rgbSource.r * illumination.r, 0.0F, 1.0F), weight),
                    std::lerp(1.0F, std::clamp(rgbSource.g * illumination.g, 0.0F, 1.0F), weight),
                    std::lerp(1.0F, std::clamp(rgbSource.b * illumination.b, 0.0F, 1.0F), weight),
                    alphaSource};
            }
        },
        draw.material);
    return {position, vertex.uv, color};
}

} // namespace sb::native_render
