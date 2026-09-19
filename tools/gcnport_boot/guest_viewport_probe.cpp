// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_viewport_probe.h"

#include <cmath>
#include <cstdio>

namespace sunbright::gcnport_boot {
namespace {

// `GXSetScissor(u32 left, u32 top, u32 wd, u32 ht)` takes its four arguments in the general file
// from r3, and `GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz)` takes its
// six in the floating file from f1. Neither convention puts an argument in the other's file.
constexpr std::size_t FIRST_GENERAL_ARGUMENT = 3;
constexpr std::size_t FIRST_FLOATING_ARGUMENT = 1;

// A float that is not a whole number of pixels. Comparing against its own truncation rather than
// against a tolerance, because the question is whether the title authored a fractional region at
// all, not whether it is close to a whole one.
[[nodiscard]] bool is_whole(double value) noexcept {
    return std::isfinite(value) && value == std::trunc(value);
}

} // namespace

const char* GuestViewportProbe::entry_name(Entry entry) noexcept {
    switch (entry) {
    case Entry::Viewport:
        return "viewport";
    case Entry::Scissor:
        return "scissor";
    }
    return "unknown";
}

gcnport::HookResult GuestViewportProbe::operator()(gcnport::GuestContext& guest) {
    entries_ += 1;
    const std::uint64_t offered = budget_ != nullptr ? budget_->offered() : 0;
    drawsBefore_.add(offered);

    Rectangle rectangle{};
    if (entry_ == Entry::Scissor) {
        rectangle = {static_cast<std::int32_t>(guest.general_register(FIRST_GENERAL_ARGUMENT)),
                     static_cast<std::int32_t>(guest.general_register(FIRST_GENERAL_ARGUMENT + 1)),
                     static_cast<std::int32_t>(guest.general_register(FIRST_GENERAL_ARGUMENT + 2)),
                     static_cast<std::int32_t>(guest.general_register(FIRST_GENERAL_ARGUMENT + 3))};
    } else {
        const double left = guest.floating_register(FIRST_FLOATING_ARGUMENT);
        const double top = guest.floating_register(FIRST_FLOATING_ARGUMENT + 1);
        const double width = guest.floating_register(FIRST_FLOATING_ARGUMENT + 2);
        const double height = guest.floating_register(FIRST_FLOATING_ARGUMENT + 3);
        if (!is_whole(left) || !is_whole(top) || !is_whole(width) || !is_whole(height)) {
            fractional_ += 1;
        }
        rectangle = {static_cast<std::int32_t>(std::lround(left)),
                     static_cast<std::int32_t>(std::lround(top)),
                     static_cast<std::int32_t>(std::lround(width)),
                     static_cast<std::int32_t>(std::lround(height))};
        const double nearZ = guest.floating_register(FIRST_FLOATING_ARGUMENT + 4);
        const double farZ = guest.floating_register(FIRST_FLOATING_ARGUMENT + 5);
        if (!sawDepth_) {
            sawDepth_ = true;
            nearestZ_ = nearZ;
            farthestZ_ = farZ;
        }
        nearestZ_ = std::fmin(nearestZ_, nearZ);
        farthestZ_ = std::fmax(farthestZ_, farZ);
    }
    rectangles_.add(rectangle);

    if (reports_ < maxReports_) {
        reports_ += 1;
        std::printf("gmse01_boot:   %s %llu: after %llu draw(s) of the current frame, "
                    "%d,%d %dx%d\n",
                    entry_name(entry_), static_cast<unsigned long long>(reports_),
                    static_cast<unsigned long long>(offered), std::get<0>(rectangle),
                    std::get<1>(rectangle), std::get<2>(rectangle), std::get<3>(rectangle));
    }
    return gcnport::HookResult::call_original_once();
}

void GuestViewportProbe::report() const {
    std::printf("gmse01_boot: %s: %llu entr(ies)\n", entry_name(entry_),
                static_cast<unsigned long long>(entries_));
    if (entries_ == 0) {
        // A rectangle the title never sets and a hook that never fired report the same zero, and
        // only one of them says anything about the title.
        std::printf("gmse01_boot:   the hook installed and the title never reached it\n");
        return;
    }
    print_tally("rectangles, as left,top width x height = times set", rectangles_,
                [](const Rectangle& rectangle) {
                    std::printf("%d,%d %dx%d", std::get<0>(rectangle), std::get<1>(rectangle),
                                std::get<2>(rectangle), std::get<3>(rectangle));
                });
    print_tally("draws offered before the set", drawsBefore_, [](std::uint64_t draws) {
        std::printf("%llu", static_cast<unsigned long long>(draws));
    });
    if (entry_ == Entry::Viewport) {
        std::printf("gmse01_boot:   %llu rectangle(s) were not whole pixels; depth range %f..%f\n",
                    static_cast<unsigned long long>(fractional_), nearestZ_, farthestZ_);
    }
}

} // namespace sunbright::gcnport_boot
