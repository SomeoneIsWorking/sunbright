#pragma once

#include <sunbright/native_render/picture_context.h>

namespace sb::recomp {

[[nodiscard]] const native_render::PictureContext* current_semantic_j2d_context() noexcept;

} // namespace sb::recomp
