#pragma once

#include <sunbright/title_adapter/guest_memory.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace sb::title_adapter {

// The controller GMSE01 reads, and what it does over time.
//
// A diagnostic that can only watch a title is limited to whatever the title does unattended. For
// GMSE01 that is the attract cycle, and everything past it -- the file select, the stages, every
// menu and every line of text -- is behind a button press. This is the description of that press:
// what one `PADStatus` contains, and which one is in force at a given frame.
//
// It is the guest's own record and not a convenience: `PADRead` fills an array of these, `PADClamp`
// rewrites the sticks in place, and `JUTGamePad::read` (0x802c8b9c) walks the array reading `err`
// at +0x0A and skipping any entry that is not `PAD_ERR_NONE`. A writer that got the layout wrong
// would be silently ignored rather than wrong on screen.

inline constexpr std::size_t GUEST_PAD_STATUS_BYTES = 12;
inline constexpr std::size_t GUEST_PAD_PORTS = 4;

// `PADStatus` field offsets.
inline constexpr GuestAddress GUEST_PAD_BUTTONS = 0x00;
inline constexpr GuestAddress GUEST_PAD_STICK_X = 0x02;
inline constexpr GuestAddress GUEST_PAD_STICK_Y = 0x03;
inline constexpr GuestAddress GUEST_PAD_SUBSTICK_X = 0x04;
inline constexpr GuestAddress GUEST_PAD_SUBSTICK_Y = 0x05;
inline constexpr GuestAddress GUEST_PAD_TRIGGER_LEFT = 0x06;
inline constexpr GuestAddress GUEST_PAD_TRIGGER_RIGHT = 0x07;
inline constexpr GuestAddress GUEST_PAD_ANALOG_A = 0x08;
inline constexpr GuestAddress GUEST_PAD_ANALOG_B = 0x09;
inline constexpr GuestAddress GUEST_PAD_ERROR = 0x0A;

// `PAD_ERR_NONE`. Every other value makes `JUTGamePad::read` skip the entry.
inline constexpr std::int8_t GUEST_PAD_ERROR_NONE = 0;

// `PAD_BUTTON_*`, as the hardware packs them into the status word.
inline constexpr std::uint16_t GUEST_PAD_LEFT = 0x0001;
inline constexpr std::uint16_t GUEST_PAD_RIGHT = 0x0002;
inline constexpr std::uint16_t GUEST_PAD_DOWN = 0x0004;
inline constexpr std::uint16_t GUEST_PAD_UP = 0x0008;
inline constexpr std::uint16_t GUEST_PAD_Z = 0x0010;
inline constexpr std::uint16_t GUEST_PAD_R = 0x0020;
inline constexpr std::uint16_t GUEST_PAD_L = 0x0040;
inline constexpr std::uint16_t GUEST_PAD_A = 0x0100;
inline constexpr std::uint16_t GUEST_PAD_B = 0x0200;
inline constexpr std::uint16_t GUEST_PAD_X = 0x0400;
inline constexpr std::uint16_t GUEST_PAD_Y = 0x0800;
inline constexpr std::uint16_t GUEST_PAD_START = 0x1000;

// The trigger value a pressed shoulder reports. `L` and `R` are both a digital bit and an analogue
// depth, and a title that reads the depth would see a released trigger under a pressed bit.
inline constexpr std::uint8_t GUEST_PAD_TRIGGER_PRESSED = 0xFF;

struct GuestPadState {
    std::uint16_t buttons = 0;
    std::int8_t stickX = 0;
    std::int8_t stickY = 0;
    std::int8_t substickX = 0;
    std::int8_t substickY = 0;
    bool operator==(const GuestPadState&) const = default;
};

// Fills one `PADStatus` as the hardware would, big-endian, ready to be written into guest memory.
// The analogue triggers follow their digital bits so the two cannot disagree.
void encode_guest_pad_status(const GuestPadState& state,
                             std::array<std::uint8_t, GUEST_PAD_STATUS_BYTES>& out) noexcept;

// Names one button, for a script. Returns false for anything else rather than ignoring it: a typo
// in a script is a run that silently never pressed what it was told to.
[[nodiscard]] bool parse_guest_pad_button(std::string_view name, std::uint16_t& button) noexcept;

enum class GuestPadScriptError : std::uint8_t {
    None,
    EmptyEntry,
    MissingSeparator,
    MalformedFrame,
    FramesOutOfOrder,
    UnknownButton,
    EmptyButtonList,
};

[[nodiscard]] const char* name(GuestPadScriptError error) noexcept;

// What the pad holds at each frame of a run.
//
// A script is `<frame>:<buttons>` entries separated by whitespace, with `<buttons>` either `-` for
// nothing held or `+`-separated button names. A state holds from its frame until the next entry
// replaces it, which is what a press is: `"2400:START 2410:-"` holds Start for ten frames.
class GuestPadTimeline {
  public:
    [[nodiscard]] GuestPadScriptError parse(std::string_view script) noexcept;

    // The state in force at `frame`. Before the first entry, nothing is held.
    [[nodiscard]] GuestPadState at(std::uint64_t frame) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] std::uint64_t last_frame() const noexcept {
        return entries_.empty() ? 0 : entries_.back().frame;
    }

  private:
    struct Entry {
        std::uint64_t frame = 0;
        GuestPadState state{};
    };

    std::vector<Entry> entries_;
};

} // namespace sb::title_adapter
