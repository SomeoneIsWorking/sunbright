#pragma once

#include <cstdint>

namespace sunbright::pad {

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
