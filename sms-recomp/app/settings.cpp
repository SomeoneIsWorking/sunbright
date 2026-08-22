#include "settings.h"

#include <lucent/log.h>

#include <array>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>

namespace sb::app {
namespace {

constexpr unsigned kConfigVersion = 1;

template <typename T> struct NamedValue {
    T value;
    std::string_view config;
    std::string_view display;
};

constexpr std::array kRenderers{
    NamedValue{Renderer::Aurora, "aurora", "Aurora"},
    NamedValue{Renderer::Native, "native", "Native"},
};

constexpr std::array kFrameRates{
    NamedValue{FrameRateMode::Vanilla, "vanilla", "Vanilla"},
    NamedValue{FrameRateMode::Interpolated60, "interpolated-60", "Interpolated 60 FPS"},
    NamedValue{FrameRateMode::InterpolatedMatchRefresh, "interpolated-unlocked",
               "Interpolated Match Refresh"},
    NamedValue{FrameRateMode::Native60, "native-60", "Native 60 FPS"},
    NamedValue{FrameRateMode::NativeMatchRefresh, "native-unlocked", "Native Match Refresh"},
};

template <typename T, std::size_t N>
std::optional<T> parse_named(std::string_view text, const std::array<NamedValue<T>, N>& values) {
    for (const auto& entry : values) {
        if (entry.config == text)
            return entry.value;
    }
    return std::nullopt;
}

template <typename T, std::size_t N>
std::string_view lookup_name(T value, const std::array<NamedValue<T>, N>& values, bool display) {
    for (const auto& entry : values) {
        if (entry.value == value)
            return display ? entry.display : entry.config;
    }
    std::abort();
}

bool env_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

bool parse_line(Settings& out, std::string_view key, std::string_view value, unsigned& version) {
    if (key == "version") {
        if (value == "1") {
            version = 1;
            return true;
        }
        return false;
    }
    if (key == "renderer") {
        const auto parsed = parse_named(value, kRenderers);
        if (!parsed)
            return false;
        out.renderer = *parsed;
        return true;
    }
    if (key == "framerate") {
        const auto parsed = parse_named(value, kFrameRates);
        if (!parsed)
            return false;
        out.frameRate = *parsed;
        return true;
    }
    if (key == "haze") {
        if (value != "true" && value != "false")
            return false;
        out.hazeEnabled = value == "true";
        return true;
    }
    return false;
}

} // namespace

bool SettingsStore::load(const std::filesystem::path& path) {
    m_path = path;
    m_persisted = {};
    m_rendererOverride.reset();
    m_frameRateOverride.reset();
    m_hazeOverride.reset();
    m_nativeRendererApproved = false;

    std::ifstream input(path);
    if (input) {
        unsigned version = 0;
        std::string line;
        unsigned lineNumber = 0;
        while (std::getline(input, line)) {
            ++lineNumber;
            if (line.empty() || line.front() == '#')
                continue;
            const std::size_t separator = line.find('=');
            if (separator == std::string::npos || separator == 0 || separator + 1 >= line.size() ||
                !parse_line(m_persisted, std::string_view(line).substr(0, separator),
                            std::string_view(line).substr(separator + 1), version)) {
                lucent::error("settings", "{}:{}: invalid Sunbright setting '{}'", path.string(),
                              lineNumber, line);
                return false;
            }
        }
        if (!input.eof()) {
            lucent::error("settings", "failed while reading {}", path.string());
            return false;
        }
        if (version != kConfigVersion) {
            lucent::error("settings", "{}: missing or unsupported config version (expected {})",
                          path.string(), kConfigVersion);
            return false;
        }
    } else if (std::filesystem::exists(path)) {
        lucent::error("settings", "cannot open settings file {}", path.string());
        return false;
    }

    m_effective = m_persisted;
    apply_environment_overrides();
    return true;
}

bool SettingsStore::save() const {
    if (m_path.empty()) {
        lucent::error("settings", "cannot save settings before a path is configured");
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(m_path.parent_path(), error);
    if (error) {
        lucent::error("settings", "cannot create settings directory {}: {}",
                      m_path.parent_path().string(), error.message());
        return false;
    }

    const auto pending = m_path.string() + ".new";
    {
        std::ofstream output(pending, std::ios::trunc);
        if (!output) {
            lucent::error("settings", "cannot write settings file {}", pending);
            return false;
        }
        output << "version=" << kConfigVersion << '\n'
               << "renderer=" << config_name(m_persisted.renderer) << '\n'
               << "framerate=" << config_name(m_persisted.frameRate) << '\n'
               << "haze=" << (m_persisted.hazeEnabled ? "true" : "false") << '\n';
        output.close();
        if (!output) {
            lucent::error("settings", "failed while writing settings file {}", pending);
            return false;
        }
    }
    std::filesystem::rename(pending, m_path, error);
    if (error) {
        lucent::error("settings", "cannot install settings file {}: {}", m_path.string(),
                      error.message());
        return false;
    }
    return true;
}

void SettingsStore::set_renderer(Renderer renderer) noexcept {
    m_persisted.renderer = renderer;
    m_effective.renderer = m_rendererOverride.value_or(renderer);
}

void SettingsStore::set_frame_rate(FrameRateMode mode) noexcept {
    m_persisted.frameRate = mode;
    m_effective.frameRate = m_frameRateOverride.value_or(mode);
}

void SettingsStore::set_haze_enabled(bool enabled) noexcept {
    m_persisted.hazeEnabled = enabled;
    m_effective.hazeEnabled = m_hazeOverride.value_or(enabled);
}

void SettingsStore::apply_environment_overrides() {
    if (env_enabled("SBR_SDLGPU")) {
        m_rendererOverride = Renderer::Native;
        m_effective.renderer = *m_rendererOverride;
    }

    if (const char* value = std::getenv("SBR_FRAME_RATE"); value != nullptr && value[0] != '\0') {
        const auto parsed = parse_named(value, kFrameRates);
        if (!parsed) {
            lucent::error("settings", "SBR_FRAME_RATE has invalid value '{}'", value);
            std::abort();
        }
        m_frameRateOverride = *parsed;
        m_effective.frameRate = *m_frameRateOverride;
    } else if (env_enabled("SBR_60FPS") || env_enabled("SBR_LERP60")) {
        m_frameRateOverride = FrameRateMode::Interpolated60;
        m_effective.frameRate = *m_frameRateOverride;
    }

    if (std::getenv("SBR_HAZE") != nullptr) {
        m_hazeOverride = env_enabled("SBR_HAZE");
        m_effective.hazeEnabled = *m_hazeOverride;
    }
}

SettingsStore& settings() {
    static SettingsStore store;
    return store;
}

std::string_view config_name(Renderer renderer) noexcept {
    return lookup_name(renderer, kRenderers, false);
}

std::string_view config_name(FrameRateMode mode) noexcept {
    return lookup_name(mode, kFrameRates, false);
}

std::string_view display_name(Renderer renderer) noexcept {
    return lookup_name(renderer, kRenderers, true);
}

std::string_view display_name(FrameRateMode mode) noexcept {
    return lookup_name(mode, kFrameRates, true);
}

} // namespace sb::app
