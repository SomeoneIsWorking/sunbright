#pragma once

#include <sunbright/native_render/picture_context.h>

namespace sb {

[[nodiscard]] const native_render::PictureContext* current_native_j2d_context() noexcept;

} // namespace sb
