#pragma once

#include <cstdint>

namespace sb::native_render {

enum class SemanticFrameMode : std::uint8_t { Disabled, Audit, Preview, Invalid };

[[nodiscard]] SemanticFrameMode parse_semantic_frame_mode(const char* value) noexcept;

} // namespace sb::native_render
