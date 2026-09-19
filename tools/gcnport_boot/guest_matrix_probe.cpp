// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_matrix_probe.h"

#include <sunbright/title_adapter/guest_j2d_primitives.h>

#include <array>
#include <cstdio>
#include <span>

namespace sunbright::gcnport_boot {
namespace {

constexpr std::size_t FIRST_ARGUMENT_REGISTER = 3;
constexpr std::size_t SECOND_ARGUMENT_REGISTER = 4;

bool read_through_guest_context(sb::title_adapter::GuestAddress address,
                                std::span<std::uint8_t> destination, void* context) {
    auto* const guest = static_cast<gcnport::GuestContext*>(context);
    return guest->read_memory(address, std::as_writable_bytes(destination));
}

} // namespace

const char* GuestMatrixProbe::entry_name(Entry entry) noexcept {
    switch (entry) {
    case Entry::LoadPosMtxImm:
        return "load";
    case Entry::SetCurrentMtx:
        return "current";
    }
    return "unknown";
}

gcnport::HookResult GuestMatrixProbe::operator()(gcnport::GuestContext& guest) {
    entries_ += 1;
    if (state_ == nullptr) {
        return gcnport::HookResult::call_original_once();
    }

    if (entry_ == Entry::SetCurrentMtx) {
        const auto id = static_cast<std::uint32_t>(guest.general_register(FIRST_ARGUMENT_REGISTER));
        state_->set_current(id);
        if (reports_ < maxReports_) {
            reports_ += 1;
            std::printf("gmse01_boot: matrix row %u is now current\n", id);
        }
        return gcnport::HookResult::call_original_once();
    }

    const auto matrix = static_cast<sb::title_adapter::GuestAddress>(
        guest.general_register(FIRST_ARGUMENT_REGISTER));
    const auto id = static_cast<std::uint32_t>(guest.general_register(SECOND_ARGUMENT_REGISTER));
    const sb::title_adapter::GuestMemory memory{read_through_guest_context, &guest};
    std::array<float, 12> values{};
    if (!sb::title_adapter::read_guest_matrix(memory, matrix, values)) {
        state_->count_unreadable();
        return gcnport::HookResult::call_original_once();
    }
    sb::native_render::Matrix3x4 loaded{};
    loaded.value = values;
    state_->load(id, loaded);
    if (reports_ < maxReports_) {
        reports_ += 1;
        std::printf("gmse01_boot: matrix row %u loaded from 0x%08x: [%g %g %g %g | %g %g %g %g | "
                    "%g %g %g %g]\n",
                    id, matrix, static_cast<double>(values[0]), static_cast<double>(values[1]),
                    static_cast<double>(values[2]), static_cast<double>(values[3]),
                    static_cast<double>(values[4]), static_cast<double>(values[5]),
                    static_cast<double>(values[6]), static_cast<double>(values[7]),
                    static_cast<double>(values[8]), static_cast<double>(values[9]),
                    static_cast<double>(values[10]), static_cast<double>(values[11]));
    }
    return gcnport::HookResult::call_original_once();
}

void GuestMatrixProbe::report() const {
    std::printf("gmse01_boot: guest matrix probe (%s): %llu entr(ies)\n", entry_name(entry_),
                static_cast<unsigned long long>(entries_));
}

} // namespace sunbright::gcnport_boot
