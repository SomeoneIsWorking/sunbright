// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_pad_probe.h"

#include <array>
#include <cstdio>
#include <span>
#include <string>

namespace sunbright::gcnport_boot {
namespace {

// One pad state as a report reads it: the button mask, then the two sticks. The sticks are printed
// even when centred so that a state holding no button is still distinguishable from an unreported
// one.
std::string describe(const sb::title_adapter::GuestPadState& state) {
    std::array<char, 64> text{};
    const int written =
        std::snprintf(text.data(), text.size(), "0x%04x/(%d,%d)/(%d,%d)", state.buttons,
                      state.stickX, state.stickY, state.substickX, state.substickY);
    return written > 0 ? std::string(text.data(), static_cast<std::size_t>(written))
                       : std::string("unprintable");
}

} // namespace

gcnport::HookResult GuestPadProbe::operator()(gcnport::GuestContext& guest) {
    entries_ += 1;
    const auto array =
        static_cast<gcnport::GuestAddress>(guest.general_register(STATUS_ARRAY_REGISTER));

    const gcnport::InterpretedBlock block = guest.call_original(ORIGINAL_INSTRUCTION_BUDGET);
    originalInstructions_ += block.instruction_count;
    if (entries_ == 1 || block.instruction_count < shortestOriginal_) {
        shortestOriginal_ = block.instruction_count;
    }
    if (block.instruction_count > longestOriginal_) {
        longestOriginal_ = block.instruction_count;
    }

    const std::uint64_t frame = budget_ != nullptr ? budget_->frames() : 0;
    const sb::title_adapter::GuestPadState state = timeline_.at(frame);
    std::array<std::uint8_t, sb::title_adapter::GUEST_PAD_STATUS_BYTES> bytes{};
    sb::title_adapter::encode_guest_pad_status(state, bytes);
    if (!guest.write_memory(array, std::as_bytes(std::span(bytes)))) {
        unwritable_ += 1;
        return gcnport::HookResult::return_to_caller();
    }
    written_ += 1;
    if (statesWritten_.size() < MAX_DISTINCT_STATES || statesWritten_.contains(state)) {
        statesWritten_[state] += 1;
    } else {
        statesNotTracked_ += 1;
    }
    if (state != sb::title_adapter::GuestPadState{} && reports_ < maxReports_) {
        reports_ += 1;
        std::printf("gmse01_boot: frame %llu: holding %s on port 0\n",
                    static_cast<unsigned long long>(frame), describe(state).c_str());
    }
    return gcnport::HookResult::return_to_caller();
}

void GuestPadProbe::report() const {
    std::printf("gmse01_boot: guest pad probe: %llu read(s), %llu port-0 state(s) written, %llu "
                "unwritable\n",
                static_cast<unsigned long long>(entries_),
                static_cast<unsigned long long>(written_),
                static_cast<unsigned long long>(unwritable_));
    if (entries_ == 0) {
        std::printf("gmse01_boot:   the hook installed and the title never read a controller\n");
        return;
    }
    std::printf("gmse01_boot:   the original body cost %u..%u instruction(s) against a %u bound\n",
                shortestOriginal_, longestOriginal_, ORIGINAL_INSTRUCTION_BUDGET);
    std::printf("gmse01_boot:   pad states put in front of the title:");
    for (const auto& [state, count] : statesWritten_) {
        std::printf(" %s=%llu", describe(state).c_str(), static_cast<unsigned long long>(count));
    }
    if (statesNotTracked_ != 0) {
        std::printf(" (+%llu past %zu distinct)",
                    static_cast<unsigned long long>(statesNotTracked_), MAX_DISTINCT_STATES);
    }
    std::printf("\n");
    std::printf("gmse01_boot:   the script's last entry is at frame %llu\n",
                static_cast<unsigned long long>(timeline_.last_frame()));
}

} // namespace sunbright::gcnport_boot
