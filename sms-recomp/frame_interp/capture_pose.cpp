#include "capture_pose.h"

#include <bit>
#include <cmath>
#include <cstdio>

namespace sb::frame_interp {

CapturePoseError validate_capture_pose(const CapturePose& pose) noexcept {
    bool anyNonZero = false;
    for (const std::uint32_t bits : pose.viewBits) {
        anyNonZero = anyNonZero || bits != 0;
        if (!std::isfinite(std::bit_cast<float>(bits)))
            return CapturePoseError::NonFiniteView;
    }
    return anyNonZero ? CapturePoseError::None : CapturePoseError::ZeroView;
}

CapturePoseText format_capture_pose(const CapturePose& pose) noexcept {
    CapturePoseText text;
    text.error = validate_capture_pose(pose);
    if (text.error != CapturePoseError::None)
        return text;

    int written =
        std::snprintf(text.bytes.data(), text.bytes.size(), "tick=%u view=", pose.guestTick);
    if (written < 0 || static_cast<std::size_t>(written) >= text.bytes.size()) {
        text.error = CapturePoseError::TextOverflow;
        return text;
    }
    std::size_t offset = static_cast<std::size_t>(written);
    for (const std::uint32_t bits : pose.viewBits) {
        written =
            std::snprintf(text.bytes.data() + offset, text.bytes.size() - offset, "%08x", bits);
        if (written != 8 || offset + static_cast<std::size_t>(written) >= text.bytes.size()) {
            text.error = CapturePoseError::TextOverflow;
            return text;
        }
        offset += static_cast<std::size_t>(written);
    }
    return text;
}

const char* capture_pose_error_name(CapturePoseError error) noexcept {
    switch (error) {
    case CapturePoseError::None:
        return "none";
    case CapturePoseError::NonFiniteView:
        return "non-finite-view";
    case CapturePoseError::ZeroView:
        return "zero-view";
    case CapturePoseError::TextOverflow:
        return "text-overflow";
    }
    return "unknown";
}

} // namespace sb::frame_interp
