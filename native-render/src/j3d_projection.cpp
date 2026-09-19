#include <sunbright/native_render/j3d_projection.h>

namespace sb::native_render {
namespace {

thread_local Matrix4x4 g_currentProjection{};
thread_local bool g_hasCurrentProjection = false;

} // namespace

Matrix4x4 with_zero_to_one_clip_depth(const Matrix4x4& projection) noexcept {
    Matrix4x4 converted = projection;
    constexpr std::size_t DEPTH_ROW = 8;
    constexpr std::size_t W_ROW = 12;
    for (std::size_t column = 0; column < 4; ++column) {
        converted.value[DEPTH_ROW + column] += projection.value[W_ROW + column];
    }
    return converted;
}

void publish_j3d_projection(const Matrix4x4& projection) noexcept {
    g_currentProjection = projection;
    g_hasCurrentProjection = true;
}

void clear_j3d_projection() noexcept {
    g_currentProjection = {};
    g_hasCurrentProjection = false;
}

const Matrix4x4* current_j3d_projection() noexcept {
    return g_hasCurrentProjection ? &g_currentProjection : nullptr;
}

} // namespace sb::native_render
