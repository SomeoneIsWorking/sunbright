// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

#include "gcnport/guest_context.h"
#include "gcnport/native_hooks.h"
#include "guest_matrix_state.h"

namespace sunbright::gcnport_boot {

// Native hooks on the two GX entries that decide what an immediate-mode vertex is multiplied by:
// `GXLoadPosMtxImm` (0x80362e0c), which writes one row of matrix memory, and `GXSetCurrentMtx`
// (0x80362eec), which chooses the row. Both feed one `GuestMatrixState`.
//
// Nothing is replaced: every entry ends in `call_original_once`.
class GuestMatrixProbe {
  public:
    enum class Entry : std::uint8_t { LoadPosMtxImm, SetCurrentMtx };

    GuestMatrixProbe(Entry entry, GuestMatrixState* state, std::uint64_t max_reports) noexcept
        : entry_(entry), state_(state), maxReports_(max_reports) {}

    gcnport::HookResult operator()(gcnport::GuestContext& guest);

    void report() const;

    [[nodiscard]] static const char* entry_name(Entry entry) noexcept;

  private:
    Entry entry_ = Entry::LoadPosMtxImm;
    GuestMatrixState* state_ = nullptr;
    std::uint64_t maxReports_ = 0;
    std::uint64_t entries_ = 0;
    std::uint64_t reports_ = 0;
};

} // namespace sunbright::gcnport_boot
