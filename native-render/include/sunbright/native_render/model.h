#pragma once

#include <sunbright/native_render/picture.h>

#include <array>
#include <cstdint>
#include <span>
#include <variant>

namespace sb::native_render {

struct Vec3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    bool operator==(const Vec3&) const = default;
};

struct Vec4 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 0.0F;
    bool operator==(const Vec4&) const = default;
};

struct Matrix4x4 {
    std::array<float, 16> value{};
    bool operator==(const Matrix4x4&) const = default;
};

// Triangle-list vertex values decoded from a game model resource. Matrix selection and skinning
// are resolved by the runtime adapter before publication; no GX vertex descriptor or display-list
// state crosses this renderer-neutral boundary.
struct MeshVertex {
    Vec3 position{};
    Vec2 uv{};
    Color color{1.0F, 1.0F, 1.0F, 1.0F};
    Vec3 normal{0.0F, 0.0F, 1.0F};
    bool operator==(const MeshVertex&) const = default;
};

struct MeshResource {
    std::uint64_t resource = 0;
    std::uint64_t revision = 0;
    std::uint32_t vertexCount = 0;
    bool operator==(const MeshResource&) const = default;
};

struct MeshResourceView {
    std::uint64_t resource = 0;
    std::uint64_t revision = 0;
    std::span<const MeshVertex> vertices{};
};

enum class ModelCullMode : std::uint8_t { None, Front, Back, All };
enum class ModelDepthCompare : std::uint8_t {
    Never,
    Less,
    Equal,
    LessOrEqual,
    Greater,
    NotEqual,
    GreaterOrEqual,
    Always,
};
enum class ModelAlphaTest : std::uint8_t { PassAll, GreaterOrEqualHalf };
enum class ModelBlendMode : std::uint8_t { Replace, SourceAlpha };

// Ordinary PC raster policy. Runtime adapters derive it from high-level material objects before
// publication; packed GX registers and compatibility-renderer state never cross this boundary.
struct ModelRasterPolicy {
    ModelCullMode cull = ModelCullMode::None;
    bool depthTest = true;
    ModelDepthCompare depthCompare = ModelDepthCompare::LessOrEqual;
    bool depthWrite = true;
    ModelAlphaTest alphaTest = ModelAlphaTest::PassAll;
    ModelBlendMode blend = ModelBlendMode::Replace;
    bool operator==(const ModelRasterPolicy&) const = default;
};

// Ordinary unlit colour semantics, used by controlled GPU tests and by a runtime adapter only when
// its source material is an exact match. More material families get distinct semantic types;
// unsupported J3D programs are never squeezed into this one as an approximation.
struct UnlitColorMaterial {
    Color baseColor{1.0F, 1.0F, 1.0F, 1.0F};
    bool usesVertexColor = false;
    ModelRasterPolicy raster{};
};

// A decoded game texture modulated by authored vertex colour. Asset encoding is gone before this
// boundary; the model submission carries one ordinary RGBA image matching this descriptor.
struct UnlitTexturedMaterial {
    PictureTexture texture{};
    bool usesVertexColor = true;
    ModelRasterPolicy raster{};
};

// Ordinary view-space point light. Runtime adapters resolve the game's light owner and transform
// its world-space position before publication; no GX light object or register encoding crosses
// this boundary.
struct PointLight {
    Vec3 position{};
    Color color{1.0F, 1.0F, 1.0F, 1.0F};
    Vec3 distanceAttenuation{1.0F, 0.0F, 0.0F};
    bool operator==(const PointLight&) const = default;
};

// Ordinary directional Blinn-style specular light in view space. The game publishes its authored
// direction and shininess from the stage-light owner before any console light object is built.
struct DirectionalSpecularLight {
    Vec3 directionToLight{0.0F, 0.0F, 1.0F};
    Color color{1.0F, 1.0F, 1.0F, 1.0F};
    float shininess = 1.0F;
    bool operator==(const DirectionalSpecularLight&) const = default;
};

struct ModelLightingContext {
    std::array<PointLight, 2> pointLights{};
    std::uint8_t pointLightCount = 0;
    Color ambientColor{};
    DirectionalSpecularLight specular{};
    bool operator==(const ModelLightingContext&) const = default;
};

// One decoded texture modulated by J3D's authored per-vertex diffuse lighting. This is a semantic
// PC material: ambient and point-light values are already resolved, and its channel policy is
// expressed as the choice between material and vertex colour rather than packed console bits.
struct LitTexturedMaterial {
    PictureTexture texture{};
    Color baseColor{1.0F, 1.0F, 1.0F, 1.0F};
    Color ambientColor{0.0F, 0.0F, 0.0F, 1.0F};
    ModelLightingContext lighting{};
    float litColorWeight = 1.0F;
    bool usesVertexRgb = false;
    bool usesVertexAlpha = false;
    ModelRasterPolicy raster{};
};

// One decoded texture with ordinary diffuse and directional-specular lighting. Its authored tint
// is expressed as an affine texture operation: texture * per-vertex multiplier + additive colour.
struct TintedSpecularTexturedMaterial {
    PictureTexture texture{};
    Color baseColor{1.0F, 1.0F, 1.0F, 1.0F};
    Color ambientColor{0.0F, 0.0F, 0.0F, 1.0F};
    Color tintColor{};
    ModelLightingContext lighting{};
    ModelRasterPolicy raster{};
};

using ModelMaterial = std::variant<UnlitColorMaterial, UnlitTexturedMaterial, LitTexturedMaterial,
                                   TintedSpecularTexturedMaterial>;

struct ModelDraw {
    std::uint64_t instance = 0;
    MeshResource mesh{};
    Matrix3x4 modelView{};
    Matrix4x4 projection{};
    ModelMaterial material{UnlitColorMaterial{}};
};

struct ClipVertex {
    Vec4 position{};
    Vec2 uv{};
    Color color{};
    Color additiveColor{};
};

[[nodiscard]] bool valid(const MeshVertex& vertex) noexcept;
[[nodiscard]] bool valid(const MeshResourceView& mesh) noexcept;
[[nodiscard]] bool valid(const Matrix4x4& matrix) noexcept;
[[nodiscard]] bool valid(const ModelRasterPolicy& raster) noexcept;
[[nodiscard]] bool valid(const ModelDraw& draw) noexcept;
[[nodiscard]] const ModelRasterPolicy& raster_policy(const ModelMaterial& material) noexcept;
[[nodiscard]] const PictureTexture* material_texture(const ModelMaterial& material) noexcept;
[[nodiscard]] std::uint64_t mesh_revision(std::span<const MeshVertex> vertices) noexcept;
// J3D camera matrices produce clip depth in [-w, 0]. The renderer-neutral model contract uses
// [0, w], so each runtime adapter applies this conversion before publishing a draw.
[[nodiscard]] Matrix4x4 zero_to_one_depth_projection(Matrix4x4 negativeOneToZero) noexcept;
[[nodiscard]] ClipVertex transform_vertex(const ModelDraw& draw, const MeshVertex& vertex) noexcept;

} // namespace sb::native_render
