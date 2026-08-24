#include "native_gpu_frame_state.h"

#include <cstdlib>

namespace {

void require(bool condition) {
    if (!condition)
        std::abort();
}

} // namespace

int main() {
    NativeGpuFrameState state;
    require(!state.readable());

    state.complete_frame();
    require(state.readable());

    // The named defect: beginning a later frame must invalidate the preceding frame before any
    // upload or submit can fail. Otherwise a failed frame is reported as a fresh copy of the last
    // successful one.
    state.begin_frame();
    require(!state.readable());

    state.complete_frame();
    state.fail_frame();
    require(!state.readable());
    return 0;
}
