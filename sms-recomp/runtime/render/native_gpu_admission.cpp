#include "native_gpu_admission.h"

#include <SDL3/SDL.h>

#include <cstdlib>

NativeGpuRateLimiter::NativeGpuRateLimiter(double maximumHz) noexcept : m_maximumHz(maximumHz) {}

bool NativeGpuRateLimiter::admit(std::uint64_t nowNs) noexcept {
    if (m_maximumHz <= 0.0)
        return true;

    const auto minimumGapNs = static_cast<std::uint64_t>(1e9 / m_maximumHz);
    if (m_lastAdmissionNs != 0 && nowNs - m_lastAdmissionNs < minimumGapNs) {
        ++m_skippedFrames;
        return false;
    }
    m_lastAdmissionNs = nowNs;
    return true;
}

namespace {

NativeGpuRateLimiter& limiter() {
    static NativeGpuRateLimiter instance([] {
        const char* value = std::getenv("SBR_RENDER_MAX_HZ");
        return value != nullptr ? std::strtod(value, nullptr) : 10.0;
    }());
    return instance;
}

} // namespace

bool sbr_native_gpu_admit_frame() noexcept {
    return limiter().admit(SDL_GetTicksNS());
}

double sbr_native_gpu_maximum_hz() noexcept {
    return limiter().maximum_hz();
}

long sbr_native_gpu_skipped_frames() noexcept {
    return limiter().skipped_frames();
}
