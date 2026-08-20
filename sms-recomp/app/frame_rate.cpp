#include "frame_rate.h"

#include <cstdlib>
#include <cmath>

namespace sb::app::frame_rate {
namespace {

bool env_enabled(const char *name) noexcept {
  const char *value = std::getenv(name);
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

constexpr double kDefaultRefreshHz = 60.0;
constexpr double kSmsTickHz = 30000.0 / 1001.0;
constexpr unsigned kMaxPresentationsPerTick = 64;

double g_displayRefreshHz = kDefaultRefreshHz;
double g_presentationCredit = 0.0;
FrameRateMode g_cadenceMode = FrameRateMode::Vanilla;

} // namespace

FrameRateMode mode() noexcept { return settings().effective().frameRate; }

bool interpolates() noexcept {
  return mode() == FrameRateMode::Interpolated60 ||
         mode() == FrameRateMode::InterpolatedMatchRefresh;
}

bool interpolation_matches_refresh() noexcept {
  return mode() == FrameRateMode::InterpolatedMatchRefresh;
}

bool runs_native_game_rate() noexcept {
  return mode() == FrameRateMode::Native60 ||
         mode() == FrameRateMode::NativeMatchRefresh;
}

bool host_pacing_enabled() noexcept {
  return !env_enabled("SB_TURBO");
}

uint32_t game_retrace_count(uint32_t requested) noexcept {
  if (runs_native_game_rate())
    return 1;
  return requested;
}

void set_display_refresh_hz(double refreshHz) noexcept {
  if (!std::isfinite(refreshHz) || refreshHz < kSmsTickHz || refreshHz > 1000.0)
    refreshHz = kDefaultRefreshHz;
  if (std::fabs(g_displayRefreshHz - refreshHz) > 0.001) {
    g_displayRefreshHz = refreshHz;
    g_presentationCredit = 0.0;
  }
}

double display_refresh_hz() noexcept { return g_displayRefreshHz; }

double game_hz() noexcept {
  switch (mode()) {
  case FrameRateMode::Native60:
    return 60.0;
  case FrameRateMode::NativeMatchRefresh:
    return g_displayRefreshHz;
  default:
    return 30.0;
  }
}

double game_rate_multiplier() noexcept { return game_hz() / 30.0; }

float animation_rate_constant() noexcept {
  // BetterSunshineEngine's updateFPS writes 0.5, 1.0, or 2.0 for
  // 30/60/120 Hz. The continuous form preserves the same contract when
  // Native Match Refresh targets a display rate that is not one of those
  // three presets.
  return static_cast<float>(game_hz() / 60.0);
}

float model_gate_step() noexcept {
  // BetterSunshineEngine uses 0.01, 0.02, or 0.04 at 30/60/120 Hz.
  return static_cast<float>(0.01 * game_rate_multiplier());
}

float boid_speed_scale() noexcept {
  return static_cast<float>(1.0 / game_rate_multiplier());
}

float fixed_delta_animation_rate() noexcept { return 2.0f; }

float joint_coin_animation_rate() noexcept { return 0.5f; }

unsigned textbox_entry_frames() noexcept {
  return static_cast<unsigned>(std::lround(20.0 * game_rate_multiplier()));
}

unsigned presentation_count_for_tick() noexcept {
  const FrameRateMode currentMode = mode();
  if (currentMode != g_cadenceMode) {
    g_cadenceMode = currentMode;
    g_presentationCredit = 0.0;
  }
  if (currentMode == FrameRateMode::Interpolated60)
    return 2;
  if (currentMode != FrameRateMode::InterpolatedMatchRefresh)
    return 1;

  // SMS advances once every two NTSC fields (30000/1001 Hz). Carry the
  // fractional display-frame credit between ticks so non-integral ratios such
  // as 144/29.97 alternate between four and five emissions without drift.
  g_presentationCredit += g_displayRefreshHz / kSmsTickHz;
  unsigned count = static_cast<unsigned>(g_presentationCredit);
  g_presentationCredit -= count;
  if (count == 0)
    count = 1;
  if (count > kMaxPresentationsPerTick)
    count = kMaxPresentationsPerTick;
  return count;
}

void reset_presentation_cadence() noexcept { g_presentationCredit = 0.0; }

int64_t native_frame_period_ns() noexcept {
  return static_cast<int64_t>(std::llround(1'000'000'000.0 / game_hz()));
}

} // namespace sb::app::frame_rate
