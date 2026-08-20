#include "frame_rate.h"

#include <cstdlib>

namespace sb::app::frame_rate {
namespace {

bool env_enabled(const char *name) noexcept {
  const char *value = std::getenv(name);
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

} // namespace

FrameRateMode mode() noexcept { return settings().effective().frameRate; }

bool interpolates() noexcept {
  return mode() == FrameRateMode::Interpolated60 ||
         mode() == FrameRateMode::InterpolatedUnlocked;
}

bool interpolation_unlocked() noexcept {
  return mode() == FrameRateMode::InterpolatedUnlocked;
}

bool runs_native_game_rate() noexcept {
  return mode() == FrameRateMode::Native60 ||
         mode() == FrameRateMode::NativeUnlocked;
}

bool host_pacing_enabled() noexcept {
  return mode() != FrameRateMode::NativeUnlocked && !env_enabled("SB_TURBO");
}

uint32_t game_retrace_count(uint32_t requested) noexcept {
  if (runs_native_game_rate())
    return 1;
  return requested;
}

bool is_supported(FrameRateMode candidate) noexcept {
  return candidate != FrameRateMode::InterpolatedUnlocked;
}

const char *unsupported_reason(FrameRateMode candidate) noexcept {
  if (candidate == FrameRateMode::InterpolatedUnlocked) {
    return "Aurora's replay engine currently owns exactly one midpoint "
           "emission per simulation "
           "tick. Unlocked interpolation needs a reusable, read-only "
           "interpolation plan so it "
           "can emit several display-timed alphas without advancing object "
           "history each time.";
  }
  return nullptr;
}

} // namespace sb::app::frame_rate
