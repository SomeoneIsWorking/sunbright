// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <map>

#include <sunbright/title_adapter/guest_pad.h>

#include "frame_draw_budget.h"
#include "gcnport/guest_context.h"
#include "gcnport/native_hooks.h"

namespace sunbright::gcnport_boot {

// Presses GMSE01's buttons, so a run can leave the attract cycle.
//
// Everything this tool could observe until now was whatever the title does unattended, which is the
// attract cycle and nothing else. Three publishers -- the iris wipe, `J2DGrafContext::fillBox` and
// every resource-font glyph -- are installed and have never once been entered, because the screens
// that use them are behind a button press. gcnport configures no Serial Interface device, so the
// guest sees four empty ports and no press can arrive from outside.
//
// This sits on `PADRead` (0x80351600), which `JUTGamePad::read` calls with the address of the
// four-entry `PADStatus` array in r3. The original runs first and fills the array as the hardware
// would -- every port reporting no controller -- and this then overwrites port 0 with the state the
// script says is held at the current frame. Nothing above it is bypassed: `PADClamp`, the stick
// mode, the trigger and release edges and the repeat timers are all still the title's own.
//
// Frames are counted the same way every other probe counts them, at the frame seam, which is what a
// script's numbers mean.
class GuestPadProbe {
  public:
    // `budget` may be null, which is a run with no frame seam; the script then never advances past
    // its first entry and this says so rather than pressing at an unknown time.
    GuestPadProbe(sb::title_adapter::GuestPadTimeline timeline, const FrameDrawBudget* budget,
                  std::uint64_t max_reports) noexcept
        : timeline_(std::move(timeline)), budget_(budget), maxReports_(max_reports) {}

    gcnport::HookResult operator()(gcnport::GuestContext& guest);

    void report() const;

  private:
    // `PADRead` polls four Serial Interface channels. The bound is generous because exceeding it is
    // a hard fault rather than a truncated call, and the report prints what the body actually cost
    // so the margin is a measured number rather than a hopeful one.
    static constexpr std::uint32_t ORIGINAL_INSTRUCTION_BUDGET = 1U << 20U;
    static constexpr std::size_t STATUS_ARRAY_REGISTER = 3;
    static constexpr std::size_t MAX_DISTINCT_STATES = 32;

    sb::title_adapter::GuestPadTimeline timeline_;
    const FrameDrawBudget* budget_ = nullptr;
    std::uint64_t maxReports_ = 0;
    std::uint64_t reports_ = 0;

    std::uint64_t entries_ = 0;
    std::uint64_t written_ = 0;
    std::uint64_t unwritable_ = 0;
    std::uint64_t originalInstructions_ = 0;
    std::uint32_t shortestOriginal_ = 0;
    std::uint32_t longestOriginal_ = 0;
    // Every distinct pad state this put in front of the title, with how often. A script that
    // pressed nothing and a script whose press never reached a read produce the same silence
    // otherwise. The sticks are part of the key: a run that only walks holds no button at all, and
    // keyed on the button mask alone it would report as an idle run.
    std::map<sb::title_adapter::GuestPadState, std::uint64_t> statesWritten_;
    std::uint64_t statesNotTracked_ = 0;
};

} // namespace sunbright::gcnport_boot
