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

    require(sbr_native_gpu_parse_maximum_hz("1") == 1.0);
    require(sbr_native_gpu_parse_maximum_hz("10") == 10.0);
    require(!sbr_native_gpu_parse_maximum_hz(""));
    require(!sbr_native_gpu_parse_maximum_hz("nan"));
    require(!sbr_native_gpu_parse_maximum_hz("inf"));
    require(!sbr_native_gpu_parse_maximum_hz("0"));
    require(!sbr_native_gpu_parse_maximum_hz("-1"));
    require(!sbr_native_gpu_parse_maximum_hz("11"));
    require(!sbr_native_gpu_parse_maximum_hz("5Hz"));
    return 0;
}
