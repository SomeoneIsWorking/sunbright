// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_efb_copy_probe.h"

#include <cstdio>

namespace sunbright::gcnport_boot {
namespace {

// The PowerPC ABI puts the first four integer arguments in r3-r6, which is every argument these
// entries take: `GXCopyTex(void* destination, GXBool clear)`, `GXCopyDisp(void* destination,
// GXBool clear)`, `GXSetTexCopySrc(u16 left, u16 top, u16 width, u16 height)` and
// `GXSetCopyClear(GXColor colour, u32 depth)`.
constexpr std::size_t FIRST_ARGUMENT = 3;

} // namespace

const char* GuestEfbCopyProbe::entry_name(Entry entry) noexcept {
    switch (entry) {
    case Entry::CopyToTexture:
        return "copy to texture";
    case Entry::CopyToDisplay:
        return "copy to display";
    case Entry::SetTextureSource:
        return "set texture copy source";
    case Entry::SetClear:
        return "set copy clear";
    }
    return "unknown";
}

gcnport::HookResult GuestEfbCopyProbe::operator()(gcnport::GuestContext& guest) {
    entries_ += 1;
    const std::uint64_t offered = budget_ != nullptr ? budget_->offered() : 0;
    if (entry_ == Entry::CopyToTexture || entry_ == Entry::CopyToDisplay) {
        drawsBefore_.add(offered);
        const bool cleared = guest.general_register(FIRST_ARGUMENT + 1) != 0;
        if (cleared) {
            clearing_ += 1;
        }
        // Only the copies into a texture end a pass here. A copy to the display is the frame
        // itself, and the frame seam already seals and reopens it; ending a pass at that point
        // would drop the visible image before anything encoded it.
        if (entry_ == Entry::CopyToTexture && renderer_ != nullptr) {
            renderer_->end_offscreen_pass(cleared);
        }
    }
    if (entry_ == Entry::SetTextureSource) {
        const auto left = static_cast<std::uint32_t>(guest.general_register(FIRST_ARGUMENT));
        const auto top = static_cast<std::uint32_t>(guest.general_register(FIRST_ARGUMENT + 1));
        const auto width = static_cast<std::uint32_t>(guest.general_register(FIRST_ARGUMENT + 2));
        const auto height = static_cast<std::uint32_t>(guest.general_register(FIRST_ARGUMENT + 3));
        const std::pair<std::uint32_t, std::uint32_t> origin{(left << 16U) | top,
                                                             (width << 16U) | height};
        regions_.add(origin);
    }
    if (reports_ < maxReports_) {
        reports_ += 1;
        std::printf(
            "gmse01_boot:   %s %llu: after %llu draw(s) of the current frame, "
            "r3=0x%08x r4=0x%08x r5=0x%08x r6=0x%08x\n",
            entry_name(entry_), static_cast<unsigned long long>(reports_),
            static_cast<unsigned long long>(offered), guest.general_register(FIRST_ARGUMENT),
            guest.general_register(FIRST_ARGUMENT + 1), guest.general_register(FIRST_ARGUMENT + 2),
            guest.general_register(FIRST_ARGUMENT + 3));
    }
    return gcnport::HookResult::call_original_once();
}

void GuestEfbCopyProbe::report() const {
    std::printf("gmse01_boot: %s: %llu entr(ies)\n", entry_name(entry_),
                static_cast<unsigned long long>(entries_));
    if (entries_ == 0) {
        // Said out loud, because a copy the title never makes and a hook that never fired report
        // the same zero, and only one of them is a finding about the title.
        std::printf("gmse01_boot:   the hook installed and the title never reached it\n");
        return;
    }
    if (entry_ == Entry::CopyToTexture || entry_ == Entry::CopyToDisplay) {
        std::printf("gmse01_boot:   %llu cleared the buffer afterwards\n",
                    static_cast<unsigned long long>(clearing_));
        print_tally("draws offered before the copy", drawsBefore_, [](std::uint64_t draws) {
            std::printf("%llu", static_cast<unsigned long long>(draws));
        });
    }
    if (entry_ == Entry::SetTextureSource) {
        print_tally("source regions, as left,top width x height = times read", regions_,
                    [](const std::pair<std::uint32_t, std::uint32_t>& region) {
                        std::printf("%u,%u %ux%u", region.first >> 16U, region.first & 0xFFFFU,
                                    region.second >> 16U, region.second & 0xFFFFU);
                    });
    }
}

} // namespace sunbright::gcnport_boot
