#include "native_gpu_admission.h"

#include <SDL3/SDL.h>

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>

#include <lucent/log.h>

namespace {

constexpr double kMaximumDiagnosticHz = 10.0;

} // namespace

NativeGpuRateLimiter::NativeGpuRateLimiter(double maximumHz) noexcept : m_maximumHz(maximumHz) {}

bool NativeGpuRateLimiter::admit(std::uint64_t nowNs) noexcept {
    const auto minimumGapNs = static_cast<std::uint64_t>(1e9 / m_maximumHz);
    if (m_lastAdmissionNs != 0 && nowNs - m_lastAdmissionNs < minimumGapNs) {
        ++m_skippedFrames;
        return false;
    }
    m_lastAdmissionNs = nowNs;
    return true;
}

namespace {

std::optional<double> parse_maximum_hz(std::string_view value) noexcept {
    if (value.empty())
        return std::nullopt;
    const std::string text(value);
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text.c_str(), &end);
    if (errno != 0 || end == text.c_str() || *end != '\0' || !std::isfinite(parsed) ||
        parsed <= 0.0 || parsed > kMaximumDiagnosticHz)
        return std::nullopt;
    return parsed;
}

NativeGpuRateLimiter& limiter() {
    static NativeGpuRateLimiter instance([] {
        const char* value = std::getenv("SBR_RENDER_MAX_HZ");
        if (value == nullptr)
            return kMaximumDiagnosticHz;
        const auto parsed = parse_maximum_hz(value);
        if (!parsed) {
            lucent::error("nrender",
                          "SBR_RENDER_MAX_HZ='{}' is invalid; diagnostic readback must be finite "
                          "and greater than 0 through {} Hz",
                          value, kMaximumDiagnosticHz);
            std::abort();
        }
        return *parsed;
    }());
    return instance;
}

} // namespace

std::optional<double> sbr_native_gpu_parse_maximum_hz(std::string_view value) noexcept {
    return parse_maximum_hz(value);
}

bool sbr_native_gpu_admit_frame() noexcept {
    return limiter().admit(SDL_GetTicksNS());
}

double sbr_native_gpu_maximum_hz() noexcept {
    return limiter().maximum_hz();
}

long sbr_native_gpu_skipped_frames() noexcept {
    return limiter().skipped_frames();
}
