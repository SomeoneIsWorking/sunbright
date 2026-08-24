#include "native_gpu_admission.h"

#include <cstdlib>

namespace {

void require(bool condition) {
    if (!condition)
        std::abort();
}

} // namespace

int main() {
    NativeGpuRateLimiter limiter(10.0);
    require(limiter.admit(1'000'000'000));
    require(!limiter.admit(1'050'000'000));
    require(limiter.admit(1'100'000'000));
    require(limiter.skipped_frames() == 1);

    NativeGpuRateLimiter unlimited(0.0);
    require(unlimited.admit(1));
    require(unlimited.admit(2));
    require(unlimited.skipped_frames() == 0);
    return 0;
}
