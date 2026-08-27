#include "frame_interp/capture_pose.h"

#include <bit>
#include <cstdio>
#include <string_view>

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expression);  \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

int main() {
    using sb::frame_interp::CapturePose;
    using sb::frame_interp::CapturePoseError;
    using sb::frame_interp::format_capture_pose;
    using sb::frame_interp::validate_capture_pose;

    CapturePose pose;
    pose.guestTick = 1602;
    const float view[12] = {1.0f, 0.0f,  0.0f, 12.5f, 0.0f, 1.0f,
                            0.0f, -3.0f, 0.0f, 0.0f,  1.0f, 2048.0f};
    for (std::size_t index = 0; index < pose.viewBits.size(); ++index)
        pose.viewBits[index] = std::bit_cast<std::uint32_t>(view[index]);

    CHECK(validate_capture_pose(pose) == CapturePoseError::None);
    const auto text = format_capture_pose(pose);
    CHECK(text.error == CapturePoseError::None);
    CHECK(std::string_view(text.bytes.data()) ==
          "tick=1602 view=3f800000000000000000000041480000000000003f80000000000000c0400000"
          "00000000000000003f80000045000000");

    CapturePose zero;
    CHECK(validate_capture_pose(zero) == CapturePoseError::ZeroView);

    CapturePose nonFinite = pose;
    nonFinite.viewBits[5] = 0x7F800000;
    CHECK(validate_capture_pose(nonFinite) == CapturePoseError::NonFiniteView);

    CapturePose changed = pose;
    changed.viewBits[3] = std::bit_cast<std::uint32_t>(13.5f);
    CHECK(format_capture_pose(changed).bytes != text.bytes);
}
