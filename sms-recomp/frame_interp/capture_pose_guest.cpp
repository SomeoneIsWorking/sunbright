#include "capture_pose.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdlib>

namespace sb::frame_interp {
namespace {

constexpr std::uint32_t kJ3dSysViewMatrix = 0x804045DC;

bool capture_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SBR_CAPTURE_POSE");
        return value != nullptr && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

} // namespace

void emit_capture_pose(std::uint32_t guestTick) {
    if (!capture_enabled())
        return;
    if (!sb_ram_fast(kJ3dSysViewMatrix) ||
        !sb_ram_fast(kJ3dSysViewMatrix +
                     static_cast<std::uint32_t>(kCapturePoseViewWords * 4 - 1))) {
        lucent::info("capturepose", "invalid tick={} reason=unreadable-j3dsys-view", guestTick);
        return;
    }

    CapturePose pose;
    pose.guestTick = guestTick;
    for (std::size_t index = 0; index < pose.viewBits.size(); ++index)
        pose.viewBits[index] = sb_r32(kJ3dSysViewMatrix + static_cast<std::uint32_t>(index * 4));

    const CapturePoseText text = format_capture_pose(pose);
    if (text.error != CapturePoseError::None) {
        lucent::info("capturepose", "invalid tick={} reason={}", guestTick,
                     capture_pose_error_name(text.error));
        return;
    }
    lucent::info("capturepose", "{}", text.bytes.data());
}

} // namespace sb::frame_interp
