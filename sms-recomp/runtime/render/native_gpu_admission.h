#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

class NativeGpuRateLimiter {
  public:
    explicit NativeGpuRateLimiter(double maximumHz) noexcept;

    bool admit(std::uint64_t nowNs) noexcept;
    double maximum_hz() const noexcept { return m_maximumHz; }
    long skipped_frames() const noexcept { return m_skippedFrames; }

  private:
    double m_maximumHz;
    std::uint64_t m_lastAdmissionNs = 0;
    long m_skippedFrames = 0;
};

std::optional<double> sbr_native_gpu_parse_maximum_hz(std::string_view value) noexcept;

// Diagnostic-readback singleton used by the renderer. Visible presentation bypasses it. The class
// above exposes the same policy to the CPU control test without duplicating its timing rule.
bool sbr_native_gpu_admit_frame() noexcept;
double sbr_native_gpu_maximum_hz() noexcept;
long sbr_native_gpu_skipped_frames() noexcept;
