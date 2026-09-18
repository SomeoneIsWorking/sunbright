#pragma once

#include <sunbright/native_render/model.h>

namespace sb::native_render {

// The projection in force when a draw is composed. Both runtime adapters publish the matrix their
// title handed to the hardware, and a `ModelDraw` takes its copy from here, so the renderer never
// reconstructs a projection from viewport or frustum parameters it would have to guess at.
//
// It is published per thread and per set, not per draw: a title sets a projection once and then
// draws many shapes under it. `current_j3d_projection` answers null until one has been set, which
// is the honest answer for a draw composed before any projection was published rather than an
// identity matrix that would look like a valid orthographic one.
void publish_j3d_projection(const Matrix4x4& projection) noexcept;
void clear_j3d_projection() noexcept;
[[nodiscard]] const Matrix4x4* current_j3d_projection() noexcept;

} // namespace sb::native_render
