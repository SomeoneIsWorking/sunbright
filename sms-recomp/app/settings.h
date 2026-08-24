#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace sb::app {

enum class Renderer {
    Aurora,
    Native,
};

enum class FrameRateMode {
    Vanilla,
    Interpolated60,
    InterpolatedMatchRefresh,
    Native60,
    NativeMatchRefresh,
};

struct Settings {
    Renderer renderer = Renderer::Aurora;
    FrameRateMode frameRate = FrameRateMode::Vanilla;
    bool hazeEnabled = true; // TShimmer heat haze (see docs/60fps/screen_effects.md)
};

class SettingsStore {
  public:
    bool load(const std::filesystem::path& path);
    bool save() const;

    const Settings& persisted() const noexcept { return m_persisted; }
    const Settings& effective() const noexcept { return m_effective; }

    void set_renderer(Renderer renderer) noexcept;
    void set_frame_rate(FrameRateMode mode) noexcept;
    void set_haze_enabled(bool enabled) noexcept;

    const std::filesystem::path& path() const noexcept { return m_path; }

  private:
    void apply_environment_overrides();

    std::filesystem::path m_path;
    Settings m_persisted;
    Settings m_effective;
    std::optional<Renderer> m_rendererOverride;
    std::optional<FrameRateMode> m_frameRateOverride;
    std::optional<bool> m_hazeOverride;
};

SettingsStore& settings();

std::string_view config_name(Renderer renderer) noexcept;
std::string_view config_name(FrameRateMode mode) noexcept;
std::string_view display_name(Renderer renderer) noexcept;
std::string_view display_name(FrameRateMode mode) noexcept;

} // namespace sb::app
