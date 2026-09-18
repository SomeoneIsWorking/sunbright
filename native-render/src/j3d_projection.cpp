#include <sunbright/native_render/j3d_projection.h>

namespace sb::native_render {
namespace {

thread_local Matrix4x4 g_currentProjection{};
thread_local bool g_hasCurrentProjection = false;

} // namespace

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
