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
// Converts a console projection's clip depth into the one this renderer's pipelines use.
//
// The console's projection maps the near plane to clip z = -w and the far plane to z = 0; both the
// frustum and the orthographic builders do, because one depth unit serves one hardware. This
// renderer's pipelines clip and test depth over [0, w]. The two ranges are the same size and one
// unit of w apart, so adding the w row to the z row converts one into the other exactly: near
// becomes 0 and far becomes 1, with no scale and nothing chosen. x, y and w are untouched, so
// coverage, perspective and the vertex's own w are exactly what the title authored.
//
// Getting this wrong is not a depth-ordering defect alone. The near plane is where a pipeline
// clips, so a projection in the wrong convention has the renderer clipping at the title's far
// plane and not at its near plane at all -- geometry at or behind the camera survives instead of
// being removed, and arrives smeared across the frame with an unbounded projected position.
[[nodiscard]] Matrix4x4 with_zero_to_one_clip_depth(const Matrix4x4& projection) noexcept;

// Publishes a projection already in this renderer's convention. Callers reading a console matrix
// pass it through `with_zero_to_one_clip_depth` first; this does not convert, because a projection
// this renderer composed itself would then be converted twice with no way to tell.
void publish_j3d_projection(const Matrix4x4& projection) noexcept;
void clear_j3d_projection() noexcept;
[[nodiscard]] const Matrix4x4* current_j3d_projection() noexcept;

} // namespace sb::native_render
