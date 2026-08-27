#pragma once

#include <array>
#include <cstdint>

namespace sb::frame_interp {

inline constexpr std::size_t kCapturePoseViewWords = 12;
inline constexpr std::size_t kCapturePoseTextCapacity = 128;

struct CapturePose {
    std::uint32_t guestTick = 0;
    std::array<std::uint32_t, kCapturePoseViewWords> viewBits{};

    bool operator==(const CapturePose&) const = default;
};

enum class CapturePoseError {
    None,
    NonFiniteView,
    ZeroView,
    TextOverflow,
};

struct CapturePoseText {
    std::array<char, kCapturePoseTextCapacity> bytes{};
    CapturePoseError error = CapturePoseError::None;
};

CapturePoseError validate_capture_pose(const CapturePose& pose) noexcept;
CapturePoseText format_capture_pose(const CapturePose& pose) noexcept;
const char* capture_pose_error_name(CapturePoseError error) noexcept;

// Emit the exact settled j3dSys view matrix keyed by the guest retrace tick. This is the same
// matrix handed to Aurora's stream interpolation extension, so the comparison validates the
// camera that owned the draws rather than inferring a viewpoint from pixels.
void emit_capture_pose(std::uint32_t guestTick);

} // namespace sb::frame_interp
