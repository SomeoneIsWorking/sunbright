#include "config.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string_view>

namespace sb {
namespace {

RuntimeConfig g_config;
bool g_configured = false;

std::string read_text(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string{value};
}

bool read_flag(const char* name) {
    const std::string value = read_text(name);
    return !value.empty() && value != "0";
}

template <typename Value>
bool read_integer(const char* name, Value fallback, Value minimum, Value maximum, Value& output,
                  std::string& error) {
    const std::string text = read_text(name);
    if (text.empty()) {
        output = fallback;
        return true;
    }
    Value parsed{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed < minimum ||
        parsed > maximum) {
        error = std::string{name} + " requires an integer from " + std::to_string(minimum) +
                " through " + std::to_string(maximum);
        return false;
    }
    output = parsed;
    return true;
}

bool read_rate(const char* name, double fallback, double& output, std::string& error) {
    const std::string text = read_text(name);
    if (text.empty()) {
        output = fallback;
        return true;
    }
    const char* begin = text.c_str();
    char* end = nullptr;
    const double parsed = std::strtod(begin, &end);
    if (end != begin + text.size() || !std::isfinite(parsed) || parsed < 0.0 || parsed > 1000.0) {
        error = std::string{name} + " requires a number from 0 through 1000";
        return false;
    }
    output = parsed;
    return true;
}

bool read_run_limit(std::uint64_t& output, std::string& error) {
    const std::string text = read_text("SB_QUIT_AFTER");
    if (text.empty()) {
        output = 0;
        return true;
    }
    std::uint64_t parsed{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed == 0 ||
        parsed > 10'000) {
        error = "SB_QUIT_AFTER requires an integer from 1 through 10000";
        return false;
    }
    output = parsed;
    return true;
}

void enable_compatibility_channel(std::string& channels, bool enabled, std::string_view channel) {
    if (!enabled)
        return;
    if (!channels.empty())
        channels += ',';
    channels += channel;
}

} // namespace

bool configure_runtime(std::string& error) {
    if (g_configured) {
        error = "runtime configuration was initialized more than once";
        return false;
    }

    RuntimeConfig parsed;
    if (!read_integer("SB_W", std::uint32_t{1280}, std::uint32_t{1}, std::uint32_t{16384},
                      parsed.windowWidth, error) ||
        !read_integer("SB_H", std::uint32_t{960}, std::uint32_t{1}, std::uint32_t{16384},
                      parsed.windowHeight, error) ||
        !read_run_limit(parsed.quitAfter, error) ||
        !read_integer("SB_WATCHDOG_SECS", unsigned{5}, unsigned{1}, unsigned{3600},
                      parsed.watchdogSeconds, error) ||
        !read_rate("SB_MAX_PRESENT_HZ", 120.0, parsed.maxPresentHz, error)) {
        return false;
    }

    parsed.turbo = read_flag("SB_TURBO");
    parsed.traceSequence = read_flag("SB_TRACE_SEQ");
    parsed.audioDebug = read_flag("SB_DBG_AUDIO");
    parsed.headless = read_flag("SB_HEADLESS");
    parsed.fifoCopySync = !read_flag("SB_FIFO_NO_COPYSYN");
    parsed.wipeDebug = read_flag("SB_WIPE_DBG");
    parsed.romPath = read_text("SUNBRIGHT_ROM");
    if (parsed.romPath.empty())
        parsed.romPath = "rom.rvz";
    parsed.fifoReplayPath = read_text("SB_FIFO_REPLAY");
    parsed.semanticFrameMode = read_text("SB_SEMANTIC_FRAME_MODE");
    parsed.audioRawPath = read_text("SB_AUDIO_RAW");
    parsed.pinStatePath = read_text("SB_PIN_STATE");
    parsed.padScript = read_text("SB_PAD_SCRIPT");
    parsed.logChannels = read_text("SB_LOG");
    enable_compatibility_channel(parsed.logChannels, parsed.traceSequence, "trace");
    enable_compatibility_channel(parsed.logChannels, parsed.audioDebug, "audio");
    enable_compatibility_channel(parsed.logChannels, parsed.audioDebug, "jas-native");
    enable_compatibility_channel(parsed.logChannels, parsed.wipeDebug, "wipe");

    g_config = std::move(parsed);
    g_configured = true;
    return true;
}

const RuntimeConfig& runtime_config() {
    if (!g_configured)
        std::abort();
    return g_config;
}

} // namespace sb
