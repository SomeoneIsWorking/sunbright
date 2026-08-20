#pragma once

#include "settings.h"

#include <cstdint>

namespace sb::app::frame_rate {

FrameRateMode mode() noexcept;
bool interpolates() noexcept;
bool interpolation_unlocked() noexcept;
bool runs_native_game_rate() noexcept;
bool host_pacing_enabled() noexcept;
uint32_t game_retrace_count(uint32_t requested) noexcept;
bool is_supported(FrameRateMode candidate) noexcept;
const char *unsupported_reason(FrameRateMode candidate) noexcept;

} // namespace sb::app::frame_rate
