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

// Ordinary unlit colour semantics, used by controlled GPU tests and by a runtime adapter only when
// its source material is an exact match. More material families get distinct semantic types;
// unsupported J3D programs are never squeezed into this one as an approximation.
struct UnlitColorMaterial {
    Color baseColor{1.0F, 1.0F, 1.0F, 1.0F};
    bool usesVertexColor = false;
};

// A decoded game texture modulated by authored vertex colour. Asset encoding is gone before this
// boundary; the model submission carries one ordinary RGBA image matching this descriptor.
struct UnlitTexturedMaterial {
    PictureTexture texture{};
    bool usesVertexColor = true;
};

using ModelMaterial = std::variant<UnlitColorMaterial, UnlitTexturedMaterial>;

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
};

[[nodiscard]] bool valid(const MeshVertex& vertex) noexcept;
[[nodiscard]] bool valid(const MeshResourceView& mesh) noexcept;
[[nodiscard]] bool valid(const ModelDraw& draw) noexcept;
[[nodiscard]] std::uint64_t mesh_revision(std::span<const MeshVertex> vertices) noexcept;
// J3D camera matrices produce clip depth in [-w, 0]. The renderer-neutral model contract uses
// [0, w], so each runtime adapter applies this conversion before publishing a draw.
[[nodiscard]] Matrix4x4 zero_to_one_depth_projection(Matrix4x4 negativeOneToZero) noexcept;
[[nodiscard]] ClipVertex transform_vertex(const ModelDraw& draw, const MeshVertex& vertex) noexcept;

} // namespace sb::native_render
