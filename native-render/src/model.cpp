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

bool valid(const DirectionalSpecularLight& light) noexcept {
    return valid(light.directionToLight) && valid(light.color) && finite(light.shininess) &&
           light.shininess > 0.0F;
}

Color multiply(Color first, Color second) noexcept {
    return {first.r * second.r, first.g * second.g, first.b * second.b, first.a * second.a};
}

struct VertexColors {
    Color multiplicative{};
    Color additive{};
    float detailTextureWeight = 0.0F;
};

float dot(Vec3 first, Vec3 second) noexcept {
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

Vec3 normalized(Vec3 value) noexcept {
    const float length = std::sqrt(dot(value, value));
    if (length <= 0.0F)
        return {};
    return {value.x / length, value.y / length, value.z / length};
}

Vec3 transformed_normal(const Matrix3x4& matrix, Vec3 source) noexcept {
    const auto& value = matrix.value;
    const float a = value[0];
    const float b = value[1];
    const float c = value[2];
    const float d = value[4];
    const float e = value[5];
    const float f = value[6];
    const float g = value[8];
    const float h = value[9];
    const float i = value[10];
    const float determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::fabs(determinant) <= 1.0e-12F)
        return {};
    const float inverse = 1.0F / determinant;
    return normalized({
        ((e * i - f * h) * source.x + (f * g - d * i) * source.y + (d * h - e * g) * source.z) *
            inverse,
        ((c * h - b * i) * source.x + (a * i - c * g) * source.y + (b * g - a * h) * source.z) *
            inverse,
        ((b * f - c * e) * source.x + (c * d - a * f) * source.y + (a * e - b * d) * source.z) *
            inverse,
    });
}

Color diffuse_lighting(Color source, Color ambient, const ModelLightingContext& lighting,
                       Vec3 eyePosition, Vec3 normal,
                       ModelDiffuseMode mode = ModelDiffuseMode::Clamped) noexcept {
    Color illumination = ambient;
    for (std::uint8_t index = 0; index < lighting.pointLightCount; ++index) {
        const PointLight& light = lighting.pointLights[index];
        const Vec3 offset{light.position.x - eyePosition.x, light.position.y - eyePosition.y,
                          light.position.z - eyePosition.z};
        const float distance = std::sqrt(dot(offset, offset));
        if (distance <= 0.0F)
            continue;
        const Vec3 direction{offset.x / distance, offset.y / distance, offset.z / distance};
        const float dotProduct = dot(normal, direction);
        const float diffuse =
            mode == ModelDiffuseMode::Signed ? dotProduct : std::max(0.0F, dotProduct);
        const Vec3 attenuation = light.distanceAttenuation;
        const float denominator =
            attenuation.x + attenuation.y * distance + attenuation.z * distance * distance;
        if (denominator <= 0.0F)
            continue;
        const float contribution = diffuse / denominator;
        illumination.r += light.color.r * contribution;
        illumination.g += light.color.g * contribution;
        illumination.b += light.color.b * contribution;
    }
    // The game clamps accumulated illumination before multiplying the material source.
    illumination.r = std::clamp(illumination.r, 0.0F, 1.0F);
    illumination.g = std::clamp(illumination.g, 0.0F, 1.0F);
    illumination.b = std::clamp(illumination.b, 0.0F, 1.0F);
    return {source.r * illumination.r, source.g * illumination.g, source.b * illumination.b,
            source.a};
}

Color directional_specular(const DirectionalSpecularLight& light, Vec3 normal) noexcept {
    const Vec3 direction = normalized(light.directionToLight);
    if (dot(normal, direction) < 0.0F)
        return {0.0F, 0.0F, 0.0F, 1.0F};
    const Vec3 halfDirection = normalized({direction.x, direction.y, direction.z + 1.0F});
    const float cosine = std::max(0.0F, dot(normal, halfDirection));
    const float cosineSquared = cosine * cosine;
    const float halfShininess = light.shininess * 0.5F;
    const float denominator = halfShininess + (1.0F - halfShininess) * cosineSquared;
    const float contribution =
        denominator > 0.0F ? std::clamp(cosineSquared / denominator, 0.0F, 1.0F) : 0.0F;
    return {light.color.r * contribution, light.color.g * contribution,
            light.color.b * contribution, 1.0F};
}

VertexColors specular_lighting(Color diffuseSource, Color ambient, Color diffuseScale,
                               Color additiveColor, float specularScale, float alpha,
                               const ModelLightingContext& lighting, Vec3 eyePosition,
                               Vec3 normal) noexcept {
    const Color diffuse = diffuse_lighting(diffuseSource, ambient, lighting, eyePosition, normal);
    const Color specular = directional_specular(lighting.specular, normal);
    return {
        .multiplicative =
            {
                diffuse.r * diffuseScale.r,
                diffuse.g * diffuseScale.g,
                diffuse.b * diffuseScale.b,
                0.0F,
            },
        .additive =
            {
                additiveColor.r + specularScale * specular.r,
                additiveColor.g + specularScale * specular.g,
                additiveColor.b + specularScale * specular.b,
                alpha,
            },
    };
}

} // namespace

bool valid(const Matrix4x4& matrix) noexcept {
    return std::ranges::all_of(matrix.value, finite);
}

bool valid(const ModelLightingContext& lighting) noexcept {
    return valid(lighting.ambientColor) && valid(lighting.specular) &&
           lighting.pointLightCount <= lighting.pointLights.size() &&
           std::ranges::all_of(lighting.pointLights.begin(),
                               lighting.pointLights.begin() + lighting.pointLightCount,
                               [](const PointLight& light) { return valid(light); });
}

void tint_directional_specular(ModelLightingContext& lighting, Color tint) noexcept {
    lighting.specular.color.r *= tint.r;
    lighting.specular.color.g *= tint.g;
    lighting.specular.color.b *= tint.b;
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
    const auto appendByte = [&](std::uint8_t value) {
        hash ^= value;
        hash *= kPrime;
    };
    for (const MeshVertex& vertex : vertices) {
        append(vertex.position.x);
        append(vertex.position.y);
        append(vertex.position.z);
        for (const Vec2 uv : {vertex.uv, vertex.uv1, vertex.uv2, vertex.uv3}) {
            append(uv.x);
            append(uv.y);
        }
        append(vertex.color.r);
        append(vertex.color.g);
        append(vertex.color.b);
        append(vertex.color.a);
        append(vertex.normal.x);
        append(vertex.normal.y);
        append(vertex.normal.z);
        appendByte(vertex.matrixIndex);
    }
    return hash;
}

Matrix4x4 zero_to_one_depth_projection(Matrix4x4 projection) noexcept {
    for (std::size_t column = 0; column < 4; ++column)
        projection.value[8 + column] += projection.value[12 + column];
    return projection;
}

bool valid(const MeshVertex& vertex) noexcept {
    const std::array textureCoordinates{vertex.uv, vertex.uv1, vertex.uv2, vertex.uv3};
    return finite(vertex.position.x) && finite(vertex.position.y) && finite(vertex.position.z) &&
           std::ranges::all_of(textureCoordinates,
                               [](Vec2 uv) { return finite(uv.x) && finite(uv.y); }) &&
           valid(vertex.color) && valid(vertex.normal) && vertex.matrixIndex < kMaxModelMatrices;
}

bool valid(const MeshResourceView& mesh) noexcept {
    return mesh.resource != 0 && !mesh.vertices.empty() && mesh.vertices.size() % 3U == 0U &&
           std::ranges::all_of(mesh.vertices,
                               [](const MeshVertex& vertex) { return valid(vertex); });
}

bool valid(const ModelRasterPolicy& raster) noexcept {
    return raster.cull <= ModelCullMode::All && raster.depthCompare <= ModelDepthCompare::Always &&
           raster.alphaTest <= ModelAlphaTest::GreaterOrEqualHalf &&
           raster.blend <= ModelBlendMode::PremultipliedAlpha;
}

bool valid(const ModelFog& fog) noexcept {
    if (fog.mode == ModelFogMode::Disabled)
        return true;
    return fog.mode == ModelFogMode::Linear && finite(fog.start) && finite(fog.end) &&
           fog.start < fog.end && valid(fog.color);
}

const ModelRasterPolicy& raster_policy(const ModelMaterial& material) noexcept {
    return std::visit([](const auto& value) -> const ModelRasterPolicy& { return value.raster; },
                      material);
}

std::uint8_t material_texture_count(const ModelMaterial& material) noexcept {
    return std::visit(
        [](const auto& value) -> std::uint8_t {
            using Material = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<Material, LitTexturedAlphaMaskMaterial> ||
                          std::is_same_v<Material, LitLayeredTexturedMaterial> ||
                          std::is_same_v<Material, LitTintedLayeredSpecularMaterial>)
                return 2;
            if constexpr (std::is_same_v<Material, UnlitTexturedMaterial> ||
                          std::is_same_v<Material, AlphaMaskedColorMaterial> ||
                          std::is_same_v<Material, LitTexturedMaterial> ||
                          std::is_same_v<Material, LitSpecularTexturedMaterial>) {
                return 1;
            }
            return 0;
        },
        material);
}

const PictureTexture* material_texture(const ModelMaterial& material, std::uint8_t index) noexcept {
    return std::visit(
        [index](const auto& value) -> const PictureTexture* {
            using Material = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<Material, LitTexturedAlphaMaskMaterial>) {
                if (index == 0)
                    return &value.colorTexture;
                return index == 1 ? &value.alphaMaskTexture : nullptr;
            } else if constexpr (std::is_same_v<Material, LitLayeredTexturedMaterial> ||
                                 std::is_same_v<Material, LitTintedLayeredSpecularMaterial>) {
                if (index == 0)
                    return &value.baseTexture;
                return index == 1 ? &value.detailTexture : nullptr;
            } else if constexpr (std::is_same_v<Material, UnlitTexturedMaterial> ||
                                 std::is_same_v<Material, AlphaMaskedColorMaterial> ||
                                 std::is_same_v<Material, LitTexturedMaterial> ||
                                 std::is_same_v<Material, LitSpecularTexturedMaterial>) {
                return index == 0 ? &value.texture : nullptr;
            } else {
                return nullptr;
            }
        },
        material);
}

bool material_images_match(const ModelMaterial& material,
                           std::span<const DecodedImageView> images) noexcept {
    if (images.size() != material_texture_count(material))
        return false;
    for (std::size_t index = 0; index < images.size(); ++index) {
        const PictureTexture* texture =
            material_texture(material, static_cast<std::uint8_t>(index));
        const DecodedImageView& image = images[index];
        if (texture == nullptr || !valid(image) || image.resource != texture->resource ||
            image.revision != texture->revision || image.width != texture->width ||
            image.height != texture->height) {
            return false;
        }
    }
    return true;
}

bool valid(const ModelDraw& draw) noexcept {
    const bool validMaterial = std::visit(
        [](const auto& material) {
            using Material = std::remove_cvref_t<decltype(material)>;
            if constexpr (std::is_same_v<Material, UnlitColorMaterial>) {
                return valid(material.baseColor);
            } else if constexpr (std::is_same_v<Material, UnlitTexturedMaterial>) {
                return material.texture.resource != 0 && material.texture.width != 0 &&
                       material.texture.height != 0 &&
                       material.textureCoordinates <= ModelTextureCoordinates::Secondary;
            } else if constexpr (std::is_same_v<Material, AlphaMaskedColorMaterial>) {
                return material.texture.resource != 0 && material.texture.width != 0 &&
                       material.texture.height != 0 && valid(material.color) &&
                       finite(material.alphaScale) && material.alphaScale >= 0.0F;
            } else if constexpr (std::is_same_v<Material, LitColorMaterial>) {
                return valid(material.baseColor) && valid(material.ambientColor) &&
                       valid(material.lighting);
            } else if constexpr (std::is_same_v<Material, LitTexturedMaterial>) {
                return material.texture.resource != 0 && material.texture.width != 0 &&
                       material.texture.height != 0 && valid(material.baseColor) &&
                       valid(material.ambientColor) && valid(material.lighting) &&
                       finite(material.litColorWeight) && material.litColorWeight >= 0.0F &&
                       material.litColorWeight <= 1.0F;
            } else if constexpr (std::is_same_v<Material, LitTexturedAlphaMaskMaterial>) {
                return material.colorTexture.resource != 0 && material.colorTexture.width != 0 &&
                       material.colorTexture.height != 0 &&
                       material.alphaMaskTexture.resource != 0 &&
                       material.alphaMaskTexture.width != 0 &&
                       material.alphaMaskTexture.height != 0 && valid(material.baseColor) &&
                       valid(material.ambientColor) && valid(material.lighting) &&
                       finite(material.alphaScale) && material.alphaScale >= 0.0F;
            } else if constexpr (std::is_same_v<Material, LitLayeredTexturedMaterial>) {
                return material.baseTexture.resource != 0 && material.baseTexture.width != 0 &&
                       material.baseTexture.height != 0 && material.detailTexture.resource != 0 &&
                       material.detailTexture.width != 0 && material.detailTexture.height != 0 &&
                       valid(material.baseColor) && valid(material.ambientColor) &&
                       valid(material.lighting) && finite(material.detailWeight) &&
                       material.detailWeight >= 0.0F && material.detailWeight <= 1.0F &&
                       material.diffuseMode <= ModelDiffuseMode::Signed;
            } else if constexpr (std::is_same_v<Material, LitTintedLayeredSpecularMaterial>) {
                return material.baseTexture.resource != 0 && material.baseTexture.width != 0 &&
                       material.baseTexture.height != 0 && material.detailTexture.resource != 0 &&
                       material.detailTexture.width != 0 && material.detailTexture.height != 0 &&
                       valid(material.baseColor) && valid(material.ambientColor) &&
                       valid(material.effectColor) && valid(material.lighting) &&
                       material.lighting.pointLightCount == 2 && finite(material.detailWeight) &&
                       material.detailWeight >= 0.0F && material.detailWeight <= 1.0F &&
                       finite(material.layerWeight) && material.layerWeight >= 0.0F &&
                       material.layerWeight <= 1.0F;
            } else if constexpr (std::is_same_v<Material, LitSpecularColorMaterial>) {
                return valid(material.baseColor) && valid(material.ambientColor) &&
                       valid(material.diffuseScale) && material.diffuseScale.r >= 0.0F &&
                       material.diffuseScale.g >= 0.0F && material.diffuseScale.b >= 0.0F &&
                       finite(material.specularScale) && material.specularScale >= 0.0F &&
                       valid(material.lighting);
            } else {
                return material.texture.resource != 0 && material.texture.width != 0 &&
                       material.texture.height != 0 && valid(material.baseColor) &&
                       valid(material.ambientColor) && valid(material.textureDiffuseScale) &&
                       valid(material.additiveColor) && material.textureDiffuseScale.r >= 0.0F &&
                       material.textureDiffuseScale.g >= 0.0F &&
                       material.textureDiffuseScale.b >= 0.0F && material.additiveColor.r >= 0.0F &&
                       material.additiveColor.g >= 0.0F && material.additiveColor.b >= 0.0F &&
                       finite(material.specularScale) && material.specularScale >= 0.0F &&
                       valid(material.lighting);
            }
        },
        draw.material);
    const bool validPose =
        draw.pose.count != 0 && draw.pose.count <= draw.pose.modelViews.size() &&
        std::ranges::all_of(
            draw.pose.modelViews.begin(), draw.pose.modelViews.begin() + draw.pose.count,
            [](const Matrix3x4& matrix) { return std::ranges::all_of(matrix.value, finite); });
    return draw.instance != 0 && draw.mesh.resource != 0 && draw.mesh.vertexCount != 0 &&
           draw.mesh.vertexCount % 3U == 0U && validPose && valid(draw.projection) &&
           validMaterial && valid(raster_policy(draw.material)) && valid(draw.fog);
}

bool model_mesh_matches(const ModelDraw& draw, const MeshResourceView& mesh) noexcept {
    return valid(draw) && valid(mesh) && draw.mesh.resource == mesh.resource &&
           draw.mesh.revision == mesh.revision && draw.mesh.vertexCount == mesh.vertices.size() &&
           std::ranges::all_of(mesh.vertices, [&](const MeshVertex& vertex) {
               return vertex.matrixIndex < draw.pose.count;
           });
}

ModelPoseBuildResult build_model_pose(std::span<const ModelMatrixBinding> bindings, ModelPose& pose,
                                      std::span<std::uint8_t> sourceToCompact) noexcept {
    if (bindings.empty())
        return ModelPoseBuildResult::Empty;
    if (bindings.size() > kMaxModelMatrices)
        return ModelPoseBuildResult::TooManyMatrices;
    std::ranges::fill(sourceToCompact, 0xFFU);
    ModelPose result{};
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        const ModelMatrixBinding& binding = bindings[index];
        if (binding.sourceIndex >= sourceToCompact.size())
            return ModelPoseBuildResult::SourceIndexOutOfRange;
        if (sourceToCompact[binding.sourceIndex] != 0xFFU)
            return ModelPoseBuildResult::DuplicateSourceIndex;
        if (!std::ranges::all_of(binding.modelView.value, finite))
            return ModelPoseBuildResult::InvalidMatrix;
        sourceToCompact[binding.sourceIndex] = static_cast<std::uint8_t>(index);
        result.modelViews[index] = binding.modelView;
    }
    result.count = static_cast<std::uint8_t>(bindings.size());
    pose = result;
    return ModelPoseBuildResult::Success;
}

ClipVertex transform_vertex(const ModelDraw& draw, const MeshVertex& vertex) noexcept {
    const Matrix3x4& modelViewMatrix = draw.pose.modelViews[vertex.matrixIndex];
    const auto& modelView = modelViewMatrix.value;
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
    const Vec3 eyePosition{eyeX, eyeY, eyeZ};
    const Vec3 normal = transformed_normal(modelViewMatrix, vertex.normal);
    const VertexColors colors = std::visit(
        [&](const auto& material) {
            using Material = std::remove_cvref_t<decltype(material)>;
            if constexpr (std::is_same_v<Material, UnlitColorMaterial>) {
                return VertexColors{
                    .multiplicative = material.usesVertexColor
                                          ? multiply(material.baseColor, vertex.color)
                                          : material.baseColor,
                };
            } else if constexpr (std::is_same_v<Material, UnlitTexturedMaterial>) {
                return VertexColors{.multiplicative = material.usesVertexColor
                                                          ? vertex.color
                                                          : Color{1.0F, 1.0F, 1.0F, 1.0F}};
            } else if constexpr (std::is_same_v<Material, AlphaMaskedColorMaterial>) {
                return VertexColors{
                    .multiplicative = {0.0F, 0.0F, 0.0F, material.alphaScale},
                    .additive = {material.color.r, material.color.g, material.color.b, 0.0F},
                };
            } else if constexpr (std::is_same_v<Material, LitColorMaterial>) {
                const Color rgbSource = material.usesVertexRgb ? vertex.color : material.baseColor;
                const float alphaSource =
                    material.usesVertexAlpha ? vertex.color.a : material.baseColor.a;
                const Color lit = diffuse_lighting(rgbSource, material.ambientColor,
                                                   material.lighting, eyePosition, normal);
                return VertexColors{.multiplicative = {lit.r, lit.g, lit.b, alphaSource}};
            } else if constexpr (std::is_same_v<Material, LitTexturedMaterial>) {
                const Color rgbSource = material.usesVertexRgb ? vertex.color : material.baseColor;
                const float alphaSource =
                    material.usesVertexAlpha ? vertex.color.a : material.baseColor.a;
                const Color lit = diffuse_lighting(rgbSource, material.ambientColor,
                                                   material.lighting, eyePosition, normal);
                const float weight = material.litColorWeight;
                return VertexColors{.multiplicative = {
                                        std::lerp(1.0F, lit.r, weight),
                                        std::lerp(1.0F, lit.g, weight),
                                        std::lerp(1.0F, lit.b, weight),
                                        alphaSource,
                                    }};
            } else if constexpr (std::is_same_v<Material, LitTexturedAlphaMaskMaterial>) {
                const Color lit = diffuse_lighting(material.baseColor, material.ambientColor,
                                                   material.lighting, eyePosition, normal);
                return VertexColors{.multiplicative = {lit.r, lit.g, lit.b, material.alphaScale}};
            } else if constexpr (std::is_same_v<Material, LitLayeredTexturedMaterial>) {
                const Color lit =
                    diffuse_lighting(material.baseColor, material.ambientColor, material.lighting,
                                     eyePosition, normal, material.diffuseMode);
                const float litWeight = 1.0F - material.detailWeight;
                return VertexColors{
                    .multiplicative = {lit.r * litWeight, lit.g * litWeight, lit.b * litWeight,
                                       material.baseColor.a},
                    .detailTextureWeight = material.detailWeight,
                };
            } else if constexpr (std::is_same_v<Material, LitTintedLayeredSpecularMaterial>) {
                const Color diffuse =
                    diffuse_lighting(material.baseColor, material.ambientColor, material.lighting,
                                     eyePosition, normal, ModelDiffuseMode::Signed);
                const Color specular = directional_specular(material.lighting.specular, normal);
                const float diffuseWeight = 1.0F - material.detailWeight;
                return VertexColors{
                    .multiplicative =
                        {
                            material.effectColor.r + diffuseWeight * diffuse.r - 0.5F,
                            material.effectColor.g + diffuseWeight * diffuse.g - 0.5F,
                            material.effectColor.b + diffuseWeight * diffuse.b - 0.5F,
                            material.baseColor.a,
                        },
                    .additive =
                        {
                            material.layerWeight * specular.r,
                            material.layerWeight * specular.g,
                            material.layerWeight * specular.b,
                            material.layerWeight,
                        },
                    .detailTextureWeight = material.detailWeight,
                };
            } else if constexpr (std::is_same_v<Material, LitSpecularColorMaterial>) {
                const Color diffuseSource =
                    material.usesVertexRgb ? vertex.color : material.baseColor;
                return specular_lighting(diffuseSource, material.ambientColor,
                                         material.diffuseScale, {}, material.specularScale,
                                         material.baseColor.a, material.lighting, eyePosition,
                                         normal);
            } else {
                const Color diffuseSource =
                    material.usesVertexRgb ? vertex.color : material.baseColor;
                return specular_lighting(diffuseSource, material.ambientColor,
                                         material.textureDiffuseScale, material.additiveColor,
                                         material.specularScale, material.baseColor.a,
                                         material.lighting, eyePosition, normal);
            }
        },
        draw.material);
    Vec2 primaryUv = vertex.uv;
    if (const auto* unlitTexture = std::get_if<UnlitTexturedMaterial>(&draw.material);
        unlitTexture != nullptr &&
        unlitTexture->textureCoordinates == ModelTextureCoordinates::Secondary) {
        primaryUv = vertex.uv1;
    }
    return {position,
            primaryUv,
            vertex.uv1,
            vertex.uv2,
            vertex.uv3,
            colors.multiplicative,
            colors.additive,
            colors.detailTextureWeight,
            -eyeZ};
}

} // namespace sb::native_render
