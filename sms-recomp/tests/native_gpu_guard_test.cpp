#include "native_gpu_guard.h"

#include <cstdlib>

namespace {

void require(bool condition) {
    if (!condition)
        std::abort();
}

} // namespace

int main() {
    require(sbr_native_gpu_parse_fence_timeout("0") == 0.0);
    require(sbr_native_gpu_parse_fence_timeout("5") == 5.0);
    require(sbr_native_gpu_parse_fence_timeout("60") == 60.0);
    require(!sbr_native_gpu_parse_fence_timeout(""));
    require(!sbr_native_gpu_parse_fence_timeout("nan"));
    require(!sbr_native_gpu_parse_fence_timeout("inf"));
    require(!sbr_native_gpu_parse_fence_timeout("-1"));
    require(!sbr_native_gpu_parse_fence_timeout("61"));
    require(!sbr_native_gpu_parse_fence_timeout("5s"));
    return 0;
}
