#include "presentation_label.h"

#include <cstdio>

namespace sb::frame_interp {

PresentationDumpLabel presentation_dump_label(PresentationRole role, uint32_t guestTick) noexcept {
    PresentationDumpLabel label{};
    const char* roleName = role == PresentationRole::Main ? "main" : "sub";
    std::snprintf(label.data(), label.size(), "%s-t%u", roleName, guestTick);
    return label;
}

std::optional<PresentationDumpLabel> replay_sample_dump_label(uint32_t guestTick, unsigned index,
                                                              unsigned count) noexcept {
    if (count < 2 || index == 0 || index >= count)
        return std::nullopt;
    return presentation_dump_label(PresentationRole::Sub, guestTick);
}

} // namespace sb::frame_interp
