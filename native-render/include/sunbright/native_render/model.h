#pragma once

#include <sunbright/native_render/image.h>
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

// Triangle-list vertex values decoded from a game model resource. Matrix selection is expressed as
// a compact index into the draw's semantic pose; no GX matrix slot, vertex descriptor, or
// display-list state crosses this renderer-neutral boundary.
struct MeshVertex {
    Vec3 position{};
    Vec2 uv{};
    Vec2 uv1{};
    // J3D models can select any of the first four authored texture-coordinate sets. Keep them as
    // semantic vertex values so a material that needs more than two images never falls back to a
    // GX texture-generator or silently reuses another set.
    Vec2 uv2{};
    Vec2 uv3{};
    Color color{1.0F, 1.0F, 1.0F, 1.0F};
    Vec3 normal{0.0F, 0.0F, 1.0F};
    std::uint8_t matrixIndex = 0;
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
enum class ModelBlendMode : std::uint8_t {
    Replace,
    SourceAlpha,
    PremultipliedAlpha,
    Additive,
};
enum class ModelTextureCoordinates : std::uint8_t { Primary, Secondary };

enum class ModelFogMode : std::uint8_t { Disabled, Linear };

// Ordinary view-space depth fog. Runtime adapters resolve authored material fog before
// publication; console coefficient packing and range-adjustment tables do not cross this boundary.
struct ModelFog {
    ModelFogMode mode = ModelFogMode::Disabled;
    float start = 0.0F;
    float end = 1.0F;
    Color color{};
    bool operator==(const ModelFog&) const = default;
};

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
    ModelTextureCoordinates textureCoordinates = ModelTextureCoordinates::Primary;
    bool usesVertexColor = true;
    ModelRasterPolicy raster{};
};

// A decoded texture multiplied by an authored effect colour. The colour is resolved from the
// material's ordinary constant/register values before publication; no TEV program or register
// identity crosses the semantic boundary.
struct TexturedEffectMaterial {
    PictureTexture texture{};
    ModelTextureCoordinates textureCoordinates = ModelTextureCoordinates::Primary;
    Color modulation{1.0F, 1.0F, 1.0F, 1.0F};
    ModelRasterPolicy raster{};
};

// A decoded texture used only as an opacity mask for one ordinary solid colour. Texture RGB is
// deliberately ignored; alphaScale expresses authored mask amplification before the raster test.
struct AlphaMaskedColorMaterial {
    PictureTexture texture{};
    Color color{};
    float alphaScale = 1.0F;
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

// Apply an authored high-level material tint to the directional highlight. Both material
// classifiers use this owner so coloured highlights cannot drift between semantic families.
void tint_directional_specular(ModelLightingContext& lighting, Color tint) noexcept;

enum class ModelDiffuseMode : std::uint8_t { Clamped, Signed };

// Ordinary diffuse-lit colour with no texture. Runtime adapters publish the authored material or
// vertex colour choice and resolved lights; the renderer computes one lit raster colour directly.
struct LitColorMaterial {
    Color baseColor{1.0F, 1.0F, 1.0F, 1.0F};
    Color ambientColor{0.0F, 0.0F, 0.0F, 1.0F};
    ModelLightingContext lighting{};
    bool usesVertexRgb = false;
    bool usesVertexAlpha = false;
    ModelRasterPolicy raster{};
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

// One decoded colour texture modulated by ordinary diffuse lighting, plus an independently
// decoded alpha-mask texture. The mask has its own texture coordinates and never contributes RGB.
struct LitTexturedAlphaMaskMaterial {
    PictureTexture colorTexture{};
    PictureTexture alphaMaskTexture{};
    Color baseColor{1.0F, 1.0F, 1.0F, 1.0F};
    Color ambientColor{0.0F, 0.0F, 0.0F, 1.0F};
    ModelLightingContext lighting{};
    float alphaScale = 1.0F;
    ModelRasterPolicy raster{};
};

// Two decoded textures combined with one ordinary diffuse-lit colour. The base texture multiplies
// a weighted blend of the detail texture and lit colour; both texture-coordinate sets remain
// explicit, while the original console stage ordering does not cross this boundary.
struct LitLayeredTexturedMaterial {
    PictureTexture baseTexture{};
    PictureTexture detailTexture{};
    Color baseColor{1.0F, 1.0F, 1.0F, 1.0F};
    Color ambientColor{0.0F, 0.0F, 0.0F, 1.0F};
    ModelLightingContext lighting{};
    float detailWeight = 0.0F;
    ModelDiffuseMode diffuseMode = ModelDiffuseMode::Clamped;
    ModelRasterPolicy raster{};
};

// Two decoded textures combined with signed diffuse lighting, an authored animated tint, and the
// directional highlight. The renderer receives the two ordinary clamp layers directly; no console
// colour-stage selector or register identity crosses this boundary.
struct LitTintedLayeredSpecularMaterial {
    PictureTexture baseTexture{};
    PictureTexture detailTexture{};
    Color baseColor{1.0F, 1.0F, 1.0F, 1.0F};
    Color ambientColor{0.0F, 0.0F, 0.0F, 1.0F};
    Color effectColor{};
    ModelLightingContext lighting{};
    float detailWeight = 0.0F;
    float layerWeight = 0.0F;
    ModelRasterPolicy raster{};
};

// Four decoded images form a masked character surface: the mask selects the primary or alternate
// base image, then a light-ramp layer and an authored highlight are added. Console texture slots
// and colour stages have already been resolved by the J3D classifier.
struct LitMaskedToonMaterial {
    PictureTexture primaryTexture{};
    PictureTexture maskTexture{};
    PictureTexture alternateTexture{};
    PictureTexture lightRampTexture{};
    Color baseColor{1.0F, 1.0F, 1.0F, 1.0F};
    Color ambientColor{0.0F, 0.0F, 0.0F, 1.0F};
    Color staticHighlight{};
    ModelLightingContext lighting{};
    float lightRampWeight = 0.0F;
    float staticHighlightWeight = 0.0F;
    float directionalHighlightWeight = 0.0F;
    float outputAlpha = 1.0F;
    ModelRasterPolicy raster{};
};

// Texture-free diffuse plus directional-specular lighting. The renderer receives an ordinary
// diffuse tint and highlight scale; the original console stages do not cross this boundary.
struct LitSpecularColorMaterial {
    Color baseColor{1.0F, 1.0F, 1.0F, 1.0F};
    Color ambientColor{0.0F, 0.0F, 0.0F, 1.0F};
    Color diffuseScale{1.0F, 1.0F, 1.0F, 1.0F};
    float specularScale = 1.0F;
    ModelLightingContext lighting{};
    bool usesVertexRgb = false;
    ModelRasterPolicy raster{};
};

// One decoded texture with ordinary diffuse and directional-specular lighting. The authored J3D
// program is reduced to one affine PC operation: texture * diffuse * scale + additive + specular.
struct LitSpecularTexturedMaterial {
    PictureTexture texture{};
    Color baseColor{1.0F, 1.0F, 1.0F, 1.0F};
    Color ambientColor{0.0F, 0.0F, 0.0F, 1.0F};
    Color textureDiffuseScale{1.0F, 1.0F, 1.0F, 1.0F};
    Color additiveColor{};
    float specularScale = 1.0F;
    ModelLightingContext lighting{};
    bool usesVertexRgb = false;
    ModelRasterPolicy raster{};
};

using ModelMaterial =
    std::variant<UnlitColorMaterial, UnlitTexturedMaterial, TexturedEffectMaterial,
                 AlphaMaskedColorMaterial, LitColorMaterial, LitTexturedMaterial,
                 LitTexturedAlphaMaskMaterial, LitLayeredTexturedMaterial,
                 LitTintedLayeredSpecularMaterial, LitMaskedToonMaterial, LitSpecularColorMaterial,
                 LitSpecularTexturedMaterial>;

constexpr std::size_t kMaxModelMatrices = 10;

// One current skeletal pose in view space. Rigid draws have one matrix; deformed J3D meshes carry
// a compact matrix index per vertex. The palette contains semantic transforms, not GX matrix slots.
struct ModelPose {
    std::array<Matrix3x4, kMaxModelMatrices> modelViews{};
    std::uint8_t count = 0;
};

struct ModelMatrixBinding {
    std::uint32_t sourceIndex = 0;
    Matrix3x4 modelView{};
};

enum class ModelPoseBuildResult : std::uint8_t {
    Success,
    Empty,
    TooManyMatrices,
    SourceIndexOutOfRange,
    DuplicateSourceIndex,
    InvalidMatrix,
};

struct ModelDraw {
    std::uint64_t instance = 0;
    MeshResource mesh{};
    ModelPose pose{};
    Matrix4x4 projection{};
    ModelMaterial material{UnlitColorMaterial{}};
    ModelFog fog{};
};

struct ClipVertex {
    Vec4 position{};
    Vec2 uv{};
    Vec2 uv1{};
    Vec2 uv2{};
    Vec2 uv3{};
    Color color{};
    Color additiveColor{};
    float detailTextureWeight = 0.0F;
    float eyeDepth = 0.0F;
};

[[nodiscard]] bool valid(const MeshVertex& vertex) noexcept;
[[nodiscard]] bool valid(const MeshResourceView& mesh) noexcept;
[[nodiscard]] bool valid(const Matrix4x4& matrix) noexcept;
[[nodiscard]] bool valid(const ModelLightingContext& lighting) noexcept;
[[nodiscard]] bool valid(const ModelRasterPolicy& raster) noexcept;
[[nodiscard]] bool valid(const ModelFog& fog) noexcept;
[[nodiscard]] bool valid(const ModelDraw& draw) noexcept;
[[nodiscard]] bool model_mesh_matches(const ModelDraw& draw, const MeshResourceView& mesh) noexcept;
// Compacts runtime-specific source slots into a renderer palette and writes the source-to-compact
// lookup. The caller supplies the source index domain, so invalid or duplicate bindings refuse.
[[nodiscard]] ModelPoseBuildResult
build_model_pose(std::span<const ModelMatrixBinding> bindings, ModelPose& pose,
                 std::span<std::uint8_t> sourceToCompact) noexcept;
[[nodiscard]] const ModelRasterPolicy& raster_policy(const ModelMaterial& material) noexcept;
[[nodiscard]] std::uint8_t material_texture_count(const ModelMaterial& material) noexcept;
[[nodiscard]] const PictureTexture* material_texture(const ModelMaterial& material,
                                                     std::uint8_t index = 0) noexcept;
[[nodiscard]] bool material_images_match(const ModelMaterial& material,
                                         std::span<const DecodedImageView> images) noexcept;
[[nodiscard]] std::uint64_t mesh_revision(std::span<const MeshVertex> vertices) noexcept;
// J3D camera matrices produce clip depth in [-w, 0]. The renderer-neutral model contract uses
// [0, w], so each runtime adapter applies this conversion before publishing a draw.
[[nodiscard]] Matrix4x4 zero_to_one_depth_projection(Matrix4x4 negativeOneToZero) noexcept;
[[nodiscard]] ClipVertex transform_vertex(const ModelDraw& draw, const MeshVertex& vertex) noexcept;

} // namespace sb::native_render
