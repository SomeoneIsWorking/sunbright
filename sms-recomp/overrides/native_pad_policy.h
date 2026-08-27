#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace sunbright::pad {

enum class ScriptClock {
    ReadCount,
    GuestRetrace,
};

// Empty selects the historical read-count behavior. Named values are exact so a typo cannot
// silently change when scripted input begins.
std::optional<ScriptClock> parse_script_clock(std::string_view value);
std::string_view script_clock_name(ScriptClock clock);

constexpr std::int64_t select_script_key(ScriptClock clock, std::int64_t readCount,
                                         std::uint32_t guestRetrace) {
    return clock == ScriptClock::GuestRetrace ? guestRetrace : readCount;
}

constexpr std::uint16_t combine_buttons(std::uint16_t scripted, std::uint16_t host,
                                        bool scriptOnly) {
    return scriptOnly ? scripted : static_cast<std::uint16_t>(scripted | host);
}

constexpr int select_axis(int scripted, int host, bool scriptOnly, int unset = 0x8000) {
    if (scripted != unset)
        return scripted;
    return scriptOnly ? 0 : host;
}

constexpr std::uint8_t select_trigger(int scripted, std::uint8_t host, bool scriptOnly) {
    if (scripted >= 0)
        return static_cast<std::uint8_t>(scripted);
    return scriptOnly ? 0 : host;
}

} // namespace sunbright::pad
