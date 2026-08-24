#include "native_efb_copy_plan.h"

#include <climits>
#include <cstdlib>

namespace {

void require(bool condition) {
    if (!condition)
        std::abort();
}

} // namespace

int main() {
    NativeEfbCopySequence sequence;
    const auto firstEpoch = sequence.epoch();
    require(sequence.may_merge(firstEpoch));
    require(sequence.note_copy(1) == 1);
    require(!sequence.may_merge(firstEpoch));
    require(sequence.may_merge(sequence.epoch()));

    const auto full = sbr_native_efb_copy_source(0, 0, 640, 448, 640, 448);
    require(full.valid && full.width == 640 && full.height == 448);

    const auto clipped = sbr_native_efb_copy_source(630, 440, 32, 32, 640, 448);
    require(clipped.valid && clipped.x == 630 && clipped.y == 440);
    require(clipped.width == 10 && clipped.height == 8);

    // Negative controls for the former std::clamp precondition violation: when x==targetWidth or
    // y==targetHeight, the old code passed low=1, high=0. These inputs must be rejected before any
    // GPU blit is encoded.
    require(!sbr_native_efb_copy_source(640, 0, 1, 1, 640, 448).valid);
    require(!sbr_native_efb_copy_source(0, 448, 1, 1, 640, 448).valid);
    require(!sbr_native_efb_copy_source(INT_MAX, 0, INT_MAX, 1, 640, 448).valid);
    require(!sbr_native_efb_copy_source(INT_MIN, 0, INT_MAX, 1, 640, 448).valid);
    return 0;
}
