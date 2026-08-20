#include "app/frame_rate.h"
#include "app/settings.h"

#include <cassert>
#include <filesystem>
#include <fstream>

int main() {
  const std::filesystem::path root = "scratch/tests/settings";
  std::filesystem::create_directories(root);
  const auto path = root / "sunbright.ini";

  {
    std::ofstream out(path);
    out << "version=1\nrenderer=native\nframerate=native-60\n";
  }
  assert(sb::app::settings().load(path));
  assert(sb::app::settings().effective().renderer == sb::app::Renderer::Native);
  assert(sb::app::frame_rate::runs_native_game_rate());
  assert(sb::app::frame_rate::game_retrace_count(2) == 1);

  sb::app::settings().set_frame_rate(sb::app::FrameRateMode::Vanilla);
  assert(sb::app::frame_rate::game_retrace_count(0) == 0);
  assert(sb::app::frame_rate::game_retrace_count(2) == 2);

  sb::app::settings().set_renderer(sb::app::Renderer::Aurora);
  sb::app::settings().set_frame_rate(
      sb::app::FrameRateMode::InterpolatedUnlocked);
  assert(sb::app::settings().save());
  assert(sb::app::frame_rate::interpolates());
  assert(sb::app::frame_rate::interpolation_unlocked());
  assert(!sb::app::frame_rate::is_supported(
      sb::app::FrameRateMode::InterpolatedUnlocked));
  assert(sb::app::frame_rate::unsupported_reason(
             sb::app::FrameRateMode::InterpolatedUnlocked) != nullptr);

  sb::app::SettingsStore reloaded;
  assert(reloaded.load(path));
  assert(reloaded.persisted().renderer == sb::app::Renderer::Aurora);
  assert(reloaded.persisted().frameRate ==
         sb::app::FrameRateMode::InterpolatedUnlocked);

  const auto invalidPath = root / "invalid.ini";
  {
    std::ofstream out(invalidPath);
    out << "version=1\nrenderer=aurora\nframerate=made-up\n";
  }
  sb::app::SettingsStore invalid;
  assert(!invalid.load(invalidPath));
  return 0;
}
