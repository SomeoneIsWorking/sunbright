// Drives the shipping pad encoder and script timeline.
//
// The encoder's cases are about the two fields a writer gets wrong silently: `err`, which
// `JUTGamePad::read` tests before it reads anything else, and the analogue depths behind the L, R,
// A and B bits, which a title reading depth would see as released under a pressed button.
//
// The timeline's cases are about refusal. A script is typed by hand into a command line, and every
// way of mistyping one -- a missing frame, a frame that goes backwards, a button name that is not a
// button -- has to come back as a refusal rather than as a run that quietly pressed nothing.

#include <sunbright/title_adapter/guest_pad.h>

#include <array>
#include <cassert>
#include <cstdint>

namespace {

using sb::title_adapter::encode_guest_pad_status;
using sb::title_adapter::GUEST_PAD_STATUS_BYTES;
using sb::title_adapter::GuestPadScriptError;
using sb::title_adapter::GuestPadState;
using sb::title_adapter::GuestPadTimeline;
using sb::title_adapter::parse_guest_pad_button;

void encodes_the_status_the_guest_reads() {
    std::array<std::uint8_t, GUEST_PAD_STATUS_BYTES> bytes{};
    bytes.fill(0xAB);

    GuestPadState state{};
    state.buttons = sb::title_adapter::GUEST_PAD_START | sb::title_adapter::GUEST_PAD_A;
    state.stickX = -96;
    state.stickY = 64;
    encode_guest_pad_status(state, bytes);

    assert(bytes[0x00] == 0x11 && bytes[0x01] == 0x00);
    assert(bytes[0x02] == 0xA0); // -96 as the guest stores it
    assert(bytes[0x03] == 0x40);
    assert(bytes[0x04] == 0 && bytes[0x05] == 0);
    // A is pressed, so its analogue depth is too; L and R are not.
    assert(bytes[0x06] == 0 && bytes[0x07] == 0);
    assert(bytes[0x08] == 0xFF && bytes[0x09] == 0);
    // The one field that decides whether any of the rest is looked at.
    assert(bytes[0x0A] == 0);
    assert(bytes[0x0B] == 0);
}

void follows_a_shoulder_bit_with_its_depth() {
    std::array<std::uint8_t, GUEST_PAD_STATUS_BYTES> bytes{};
    GuestPadState state{};
    state.buttons = sb::title_adapter::GUEST_PAD_L | sb::title_adapter::GUEST_PAD_R;
    encode_guest_pad_status(state, bytes);
    assert(bytes[0x06] == 0xFF && bytes[0x07] == 0xFF);
    assert(bytes[0x08] == 0 && bytes[0x09] == 0);
}

void holds_each_state_until_the_next_entry() {
    GuestPadTimeline timeline;
    assert(timeline.empty());
    assert(timeline.parse("2400:START 2410:- 2500:A+B") == GuestPadScriptError::None);
    assert(timeline.size() == 3);
    assert(timeline.last_frame() == 2500);

    assert(timeline.at(0).buttons == 0);
    assert(timeline.at(2399).buttons == 0);
    assert(timeline.at(2400).buttons == sb::title_adapter::GUEST_PAD_START);
    assert(timeline.at(2409).buttons == sb::title_adapter::GUEST_PAD_START);
    assert(timeline.at(2410).buttons == 0);
    assert(timeline.at(2499).buttons == 0);
    assert(timeline.at(2500).buttons ==
           (sb::title_adapter::GUEST_PAD_A | sb::title_adapter::GUEST_PAD_B));
    // The last entry holds for the rest of the run rather than lapsing.
    assert(timeline.at(900000).buttons ==
           (sb::title_adapter::GUEST_PAD_A | sb::title_adapter::GUEST_PAD_B));
}

void accepts_any_whitespace_between_entries() {
    GuestPadTimeline timeline;
    assert(timeline.parse("  10:UP\n20:DOWN\t30:-  ") == GuestPadScriptError::None);
    assert(timeline.size() == 3);
    assert(timeline.at(25).buttons == sb::title_adapter::GUEST_PAD_DOWN);
}

void refuses_every_way_of_mistyping_a_script() {
    GuestPadTimeline timeline;
    assert(timeline.parse("") == GuestPadScriptError::EmptyEntry);
    assert(timeline.parse("   ") == GuestPadScriptError::EmptyEntry);
    assert(timeline.parse("2400") == GuestPadScriptError::MissingSeparator);
    assert(timeline.parse(":START") == GuestPadScriptError::MalformedFrame);
    assert(timeline.parse("24o0:START") == GuestPadScriptError::MalformedFrame);
    assert(timeline.parse("-1:START") == GuestPadScriptError::MalformedFrame);
    assert(timeline.parse("2400:") == GuestPadScriptError::EmptyButtonList);
    assert(timeline.parse("2400:A+") == GuestPadScriptError::EmptyButtonList);
    assert(timeline.parse("2400:START 2300:-") == GuestPadScriptError::FramesOutOfOrder);
    assert(timeline.parse("2400:START 2400:-") == GuestPadScriptError::FramesOutOfOrder);
    assert(timeline.parse("2400:STRAT") == GuestPadScriptError::UnknownButton);
    assert(timeline.parse("2400:start") == GuestPadScriptError::UnknownButton);
    // A refused script leaves the timeline as it was rather than half-applied.
    assert(timeline.empty());
    assert(timeline.parse("10:A") == GuestPadScriptError::None);
    assert(timeline.parse("10:NOPE") == GuestPadScriptError::UnknownButton);
    assert(timeline.size() == 1 && timeline.at(10).buttons == sb::title_adapter::GUEST_PAD_A);
}

void names_every_button_it_claims_to() {
    std::uint16_t bit = 0;
    assert(parse_guest_pad_button("START", bit) && bit == sb::title_adapter::GUEST_PAD_START);
    assert(parse_guest_pad_button("Z", bit) && bit == sb::title_adapter::GUEST_PAD_Z);
    assert(parse_guest_pad_button("LEFT", bit) && bit == sb::title_adapter::GUEST_PAD_LEFT);
    assert(!parse_guest_pad_button("", bit));
    assert(!parse_guest_pad_button("STARTX", bit));
}

} // namespace

int main() {
    encodes_the_status_the_guest_reads();
    follows_a_shoulder_bit_with_its_depth();
    holds_each_state_until_the_next_entry();
    accepts_any_whitespace_between_entries();
    refuses_every_way_of_mistyping_a_script();
    names_every_button_it_claims_to();
    return 0;
}
