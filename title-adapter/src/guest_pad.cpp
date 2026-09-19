#include <sunbright/title_adapter/guest_pad.h>

#include <charconv>

namespace sb::title_adapter {
namespace {

struct NamedButton {
    std::string_view name;
    std::uint16_t bit;
};

constexpr std::array<NamedButton, 12> BUTTON_NAMES{{
    {"A", GUEST_PAD_A},
    {"B", GUEST_PAD_B},
    {"X", GUEST_PAD_X},
    {"Y", GUEST_PAD_Y},
    {"Z", GUEST_PAD_Z},
    {"L", GUEST_PAD_L},
    {"R", GUEST_PAD_R},
    {"START", GUEST_PAD_START},
    {"UP", GUEST_PAD_UP},
    {"DOWN", GUEST_PAD_DOWN},
    {"LEFT", GUEST_PAD_LEFT},
    {"RIGHT", GUEST_PAD_RIGHT},
}};

constexpr std::string_view WHITESPACE = " \t\n\r";
constexpr std::string_view NOTHING_HELD = "-";

[[nodiscard]] std::uint8_t as_byte(std::int8_t value) noexcept {
    return static_cast<std::uint8_t>(value);
}

} // namespace

void encode_guest_pad_status(const GuestPadState& state,
                             std::array<std::uint8_t, GUEST_PAD_STATUS_BYTES>& out) noexcept {
    out.fill(0);
    out[GUEST_PAD_BUTTONS] = static_cast<std::uint8_t>(state.buttons >> 8U);
    out[GUEST_PAD_BUTTONS + 1] = static_cast<std::uint8_t>(state.buttons);
    out[GUEST_PAD_STICK_X] = as_byte(state.stickX);
    out[GUEST_PAD_STICK_Y] = as_byte(state.stickY);
    out[GUEST_PAD_SUBSTICK_X] = as_byte(state.substickX);
    out[GUEST_PAD_SUBSTICK_Y] = as_byte(state.substickY);
    out[GUEST_PAD_TRIGGER_LEFT] =
        (state.buttons & GUEST_PAD_L) != 0 ? GUEST_PAD_TRIGGER_PRESSED : 0;
    out[GUEST_PAD_TRIGGER_RIGHT] =
        (state.buttons & GUEST_PAD_R) != 0 ? GUEST_PAD_TRIGGER_PRESSED : 0;
    out[GUEST_PAD_ANALOG_A] = (state.buttons & GUEST_PAD_A) != 0 ? GUEST_PAD_TRIGGER_PRESSED : 0;
    out[GUEST_PAD_ANALOG_B] = (state.buttons & GUEST_PAD_B) != 0 ? GUEST_PAD_TRIGGER_PRESSED : 0;
    out[GUEST_PAD_ERROR] = as_byte(GUEST_PAD_ERROR_NONE);
}

bool parse_guest_pad_button(std::string_view name, std::uint16_t& button) noexcept {
    for (const NamedButton& known : BUTTON_NAMES) {
        if (known.name == name) {
            button = known.bit;
            return true;
        }
    }
    return false;
}

const char* name(GuestPadScriptError error) noexcept {
    switch (error) {
    case GuestPadScriptError::None:
        return "none";
    case GuestPadScriptError::EmptyEntry:
        return "empty_entry";
    case GuestPadScriptError::MissingSeparator:
        return "missing_separator";
    case GuestPadScriptError::MalformedFrame:
        return "malformed_frame";
    case GuestPadScriptError::FramesOutOfOrder:
        return "frames_out_of_order";
    case GuestPadScriptError::UnknownButton:
        return "unknown_button";
    case GuestPadScriptError::EmptyButtonList:
        return "empty_button_list";
    }
    return "unknown";
}

GuestPadScriptError GuestPadTimeline::parse(std::string_view script) noexcept {
    std::vector<Entry> parsed;
    std::size_t cursor = 0;
    while (cursor < script.size()) {
        const std::size_t start = script.find_first_not_of(WHITESPACE, cursor);
        if (start == std::string_view::npos) {
            break;
        }
        std::size_t end = script.find_first_of(WHITESPACE, start);
        if (end == std::string_view::npos) {
            end = script.size();
        }
        const std::string_view entry = script.substr(start, end - start);
        cursor = end;

        const std::size_t separator = entry.find(':');
        if (separator == std::string_view::npos) {
            return GuestPadScriptError::MissingSeparator;
        }
        const std::string_view frameText = entry.substr(0, separator);
        const std::string_view buttonText = entry.substr(separator + 1);
        if (frameText.empty()) {
            return GuestPadScriptError::MalformedFrame;
        }
        if (buttonText.empty()) {
            return GuestPadScriptError::EmptyButtonList;
        }

        Entry made;
        const char* const frameBegin = frameText.data();
        const char* const frameEnd = frameBegin + frameText.size();
        const std::from_chars_result converted = std::from_chars(frameBegin, frameEnd, made.frame);
        if (converted.ec != std::errc{} || converted.ptr != frameEnd) {
            return GuestPadScriptError::MalformedFrame;
        }
        if (!parsed.empty() && made.frame <= parsed.back().frame) {
            return GuestPadScriptError::FramesOutOfOrder;
        }

        if (buttonText != NOTHING_HELD) {
            std::size_t buttonCursor = 0;
            while (buttonCursor <= buttonText.size()) {
                std::size_t buttonEnd = buttonText.find('+', buttonCursor);
                if (buttonEnd == std::string_view::npos) {
                    buttonEnd = buttonText.size();
                }
                const std::string_view buttonName =
                    buttonText.substr(buttonCursor, buttonEnd - buttonCursor);
                if (buttonName.empty()) {
                    return GuestPadScriptError::EmptyButtonList;
                }
                std::uint16_t bit = 0;
                if (!parse_guest_pad_button(buttonName, bit)) {
                    return GuestPadScriptError::UnknownButton;
                }
                made.state.buttons |= bit;
                buttonCursor = buttonEnd + 1;
            }
        }
        parsed.push_back(made);
    }

    if (parsed.empty()) {
        return GuestPadScriptError::EmptyEntry;
    }
    entries_ = std::move(parsed);
    return GuestPadScriptError::None;
}

GuestPadState GuestPadTimeline::at(std::uint64_t frame) const noexcept {
    GuestPadState state{};
    for (const Entry& entry : entries_) {
        if (entry.frame > frame) {
            break;
        }
        state = entry.state;
    }
    return state;
}

} // namespace sb::title_adapter
