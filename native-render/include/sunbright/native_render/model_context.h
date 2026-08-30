#pragma once

#include <sunbright/native_render/model.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace sb::native_render {

enum class ProjectionKind : std::uint8_t {
    Perspective,
    Orthographic,
};

struct ModelSceneContext {
    Matrix4x4 projection{};
    ProjectionKind projectionKind = ProjectionKind::Perspective;
};

enum class J3dProjectionResult : std::uint8_t {
    Perspective,
    Orthographic,
    NonFinite,
    Unsupported,
};

// JDrama cameras write either a C_MTXPerspective/C_MTXFrustum matrix or a C_MTXOrtho matrix into
// TGraphics before dispatching a draw pass. Convert that high-level camera value to the renderer's
// [0,w] clip-depth convention and retain its semantic projection family; no GX state is consulted.
[[nodiscard]] J3dProjectionResult capture_j3d_scene_context(const Matrix4x4& gameProjection,
                                                            ModelSceneContext& context) noexcept;

// Draw traversal can nest (for example, a 3D pass entering an orthographic pass). Empty entries are
// intentional: they suppress an outer camera when the current pass has no supported projection.
class ModelSceneContextStack {
  public:
    static constexpr std::size_t kCapacity = 32;

    [[nodiscard]] bool push(const ModelSceneContext& context) noexcept;
    [[nodiscard]] bool push_empty() noexcept;
    [[nodiscard]] bool pop() noexcept;
    [[nodiscard]] const ModelSceneContext* current() const noexcept;
    [[nodiscard]] std::size_t depth() const noexcept;

  private:
    struct Entry {
        ModelSceneContext context{};
        bool hasValue = false;
    };

    std::array<Entry, kCapacity> entries_{};
    std::size_t depth_ = 0;
};

} // namespace sb::native_render
