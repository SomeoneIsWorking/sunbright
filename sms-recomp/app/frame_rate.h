#pragma once

#include "settings.h"

#include <cstdint>

namespace sb::app::frame_rate {

FrameRateMode mode() noexcept;
bool interpolates() noexcept;
bool interpolation_matches_refresh() noexcept;
bool runs_native_game_rate() noexcept;
bool host_pacing_enabled() noexcept;
uint32_t game_retrace_count(uint32_t requested) noexcept;
void set_display_refresh_hz(double refreshHz) noexcept;
double display_refresh_hz() noexcept;
double game_hz() noexcept;
double game_rate_multiplier() noexcept;
float animation_rate_constant() noexcept;
float model_gate_step() noexcept;
float boid_speed_scale() noexcept;
float fixed_delta_animation_rate() noexcept;
float joint_coin_animation_rate() noexcept;
unsigned textbox_entry_frames() noexcept;
unsigned presentation_count_for_tick() noexcept;
void reset_presentation_cadence() noexcept;
int64_t native_frame_period_ns() noexcept;

} // namespace sb::app::frame_rate
