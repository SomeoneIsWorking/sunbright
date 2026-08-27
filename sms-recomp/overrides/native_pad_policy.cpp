#include "native_pad_policy.h"

namespace sunbright::pad {

std::optional<ScriptClock> parse_script_clock(std::string_view value) {
    if (value.empty() || value == "read-count")
        return ScriptClock::ReadCount;
    if (value == "guest-retrace")
        return ScriptClock::GuestRetrace;
    return std::nullopt;
}

std::string_view script_clock_name(ScriptClock clock) {
    switch (clock) {
    case ScriptClock::ReadCount:
        return "read-count";
    case ScriptClock::GuestRetrace:
        return "guest-retrace";
    }
    return "invalid";
}

} // namespace sunbright::pad
