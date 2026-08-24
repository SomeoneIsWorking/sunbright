#include "app/frame_rate.h"
#include "app/settings.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "settings_test:%d: requirement failed: %s\n", line, expression);
        std::abort();
    }
}

// This test is built in Release as part of the shipping build. Standard assert() compiled every
// side-effecting load/save call out under NDEBUG, so the test neither exercised production nor
// established its own fixtures. Keep the existing assertions active and make failures name the
// exact invariant.
#define require(condition) require_impl((condition), #condition, __LINE__)
#define assert(condition) require(condition)

class EnvironmentGuard {
  public:
    explicit EnvironmentGuard(const char* name) : m_name(name) {
        if (const char* value = std::getenv(name); value != nullptr)
            m_original = value;
        unset();
    }

    ~EnvironmentGuard() {
        if (m_original)
            ::setenv(m_name.c_str(), m_original->c_str(), 1);
        else
            unset();
    }

    void set(const char* value) const { require(::setenv(m_name.c_str(), value, 1) == 0); }
    void unset() const { require(::unsetenv(m_name.c_str()) == 0); }

  private:
    std::string m_name;
    std::optional<std::string> m_original;
};

void require_invalid_renderer_aborts(const std::filesystem::path& path) {
    const pid_t child = ::fork();
    require(child >= 0);
    if (child == 0) {
        require(::setenv("SBR_RENDERER", "invalid", 1) == 0);
        sb::app::SettingsStore store;
        (void)store.load(path);
        ::_exit(0);
    }

    int status = 0;
    require(::waitpid(child, &status, 0) == child);
    require(WIFSIGNALED(status));
    require(WTERMSIG(status) == SIGABRT);
}

} // namespace

int main() {
    EnvironmentGuard rendererOverride("SBR_RENDERER");
    const std::filesystem::path root = "scratch/tests/settings";
    std::filesystem::create_directories(root);
    const auto path = root / "sunbright.ini";

    {
        std::ofstream out(path);
        out << "version=1\nrenderer=native\nframerate=native-60\n";
    }
    assert(sb::app::settings().load(path));
    assert(sb::app::settings().effective().renderer == sb::app::Renderer::Native);
    assert(sb::app::settings().effective().hazeEnabled == true); // default when absent
    assert(sb::app::frame_rate::runs_native_game_rate());
    assert(sb::app::frame_rate::game_retrace_count(2) == 1);
    assert(sb::app::frame_rate::game_hz() == 60.0);
    assert(sb::app::frame_rate::game_rate_multiplier() == 2.0);
    assert(sb::app::frame_rate::animation_rate_constant() == 1.0f);
    assert(sb::app::frame_rate::model_gate_step() == 0.02f);
    assert(sb::app::frame_rate::boid_speed_scale() == 0.5f);
    assert(sb::app::frame_rate::fixed_delta_animation_rate() == 2.0f);
    assert(sb::app::frame_rate::joint_coin_animation_rate() == 0.5f);
    assert(sb::app::frame_rate::textbox_entry_frames() == 40);
    assert(sb::app::frame_rate::native_frame_period_ns() == 16666667);

    sb::app::settings().set_frame_rate(sb::app::FrameRateMode::Vanilla);
    assert(sb::app::frame_rate::game_retrace_count(0) == 0);
    assert(sb::app::frame_rate::game_retrace_count(2) == 2);
    assert(sb::app::frame_rate::game_hz() == 30.0);
    assert(sb::app::frame_rate::game_rate_multiplier() == 1.0);
    assert(sb::app::frame_rate::animation_rate_constant() == 0.5f);
    assert(sb::app::frame_rate::model_gate_step() == 0.01f);
    assert(sb::app::frame_rate::boid_speed_scale() == 1.0f);
    assert(sb::app::frame_rate::textbox_entry_frames() == 20);

    sb::app::settings().set_renderer(sb::app::Renderer::Aurora);
    sb::app::settings().set_frame_rate(sb::app::FrameRateMode::InterpolatedMatchRefresh);
    sb::app::settings().set_haze_enabled(false);
    assert(sb::app::settings().save());
    assert(sb::app::settings().effective().hazeEnabled == false);
    assert(sb::app::frame_rate::interpolates());
    assert(sb::app::frame_rate::interpolation_matches_refresh());
    sb::app::frame_rate::set_display_refresh_hz(120.0);
    unsigned presentations = 0;
    for (unsigned tick = 0; tick < 1000; ++tick)
        presentations += sb::app::frame_rate::presentation_count_for_tick();
    assert(presentations == 4004);

    sb::app::frame_rate::set_display_refresh_hz(144.0);
    presentations = 0;
    for (unsigned tick = 0; tick < 1000; ++tick) {
        const unsigned count = sb::app::frame_rate::presentation_count_for_tick();
        assert(count == 4 || count == 5);
        presentations += count;
    }
    assert(presentations == 4804);

    sb::app::settings().set_frame_rate(sb::app::FrameRateMode::NativeMatchRefresh);
    sb::app::frame_rate::set_display_refresh_hz(120.0);
    assert(sb::app::frame_rate::game_hz() == 120.0);
    assert(sb::app::frame_rate::game_rate_multiplier() == 4.0);
    assert(sb::app::frame_rate::animation_rate_constant() == 2.0f);
    assert(sb::app::frame_rate::model_gate_step() == 0.04f);
    assert(sb::app::frame_rate::boid_speed_scale() == 0.25f);
    assert(sb::app::frame_rate::textbox_entry_frames() == 80);
    assert(sb::app::frame_rate::native_frame_period_ns() == 8333333);

    sb::app::frame_rate::set_display_refresh_hz(144.0);
    assert(sb::app::frame_rate::game_hz() == 144.0);
    assert(sb::app::frame_rate::game_rate_multiplier() == 4.8);
    assert(sb::app::frame_rate::animation_rate_constant() == 2.4f);
    assert(sb::app::frame_rate::model_gate_step() == 0.048f);
    assert(sb::app::frame_rate::textbox_entry_frames() == 96);
    assert(sb::app::frame_rate::native_frame_period_ns() == 6944444);

    sb::app::SettingsStore reloaded;
    assert(reloaded.load(path));
    assert(reloaded.persisted().renderer == sb::app::Renderer::Aurora);
    assert(reloaded.persisted().frameRate == sb::app::FrameRateMode::InterpolatedMatchRefresh);
    assert(reloaded.persisted().hazeEnabled == false);

    const auto invalidPath = root / "invalid.ini";
    {
        std::ofstream out(invalidPath);
        out << "version=1\nrenderer=aurora\nframerate=made-up\n";
    }
    sb::app::SettingsStore invalid;
    assert(!invalid.load(invalidPath));

    // Haze roundtrip: a valid file with haze=true parses correctly
    const auto hazePath = root / "haze.ini";
    {
        std::ofstream out(hazePath);
        out << "version=1\nrenderer=aurora\nframerate=vanilla\nhaze=true\n";
    }
    sb::app::SettingsStore hazeOk;
    assert(hazeOk.load(hazePath));
    assert(hazeOk.persisted().hazeEnabled == true);

    // Haze roundtrip: haze=false persists and reloads
    sb::app::settings().set_haze_enabled(false);
    assert(sb::app::settings().save());
    sb::app::SettingsStore hazeOff;
    assert(hazeOff.load(path));
    assert(hazeOff.persisted().hazeEnabled == false);

    // The typed environment policy is a true two-way override. It can force Aurora even when the
    // persisted choice requests Native, and changing the persisted setting during that session
    // cannot dislodge the override.
    const auto nativeRendererPath = root / "renderer-native.ini";
    {
        std::ofstream out(nativeRendererPath);
        out << "version=1\nrenderer=native\nframerate=vanilla\nhaze=true\n";
    }
    rendererOverride.set("aurora");
    sb::app::SettingsStore forcedAurora;
    require(forcedAurora.load(nativeRendererPath));
    require(forcedAurora.persisted().renderer == sb::app::Renderer::Native);
    require(forcedAurora.effective().renderer == sb::app::Renderer::Aurora);
    forcedAurora.set_renderer(sb::app::Renderer::Native);
    require(forcedAurora.persisted().renderer == sb::app::Renderer::Native);
    require(forcedAurora.effective().renderer == sb::app::Renderer::Aurora);

    // The other arm is parsed by the same renderer table and overrides an Aurora config.
    rendererOverride.set("native");
    sb::app::SettingsStore forcedNative;
    require(forcedNative.load(path));
    require(forcedNative.persisted().renderer == sb::app::Renderer::Aurora);
    require(forcedNative.effective().renderer == sb::app::Renderer::Native);

    // A malformed selector fails at the settings boundary. Falling back to the persisted renderer
    // would turn a requested four-path run into a plausible-looking run of the wrong path.
    rendererOverride.unset();
    require_invalid_renderer_aborts(path);

    return 0;
}
