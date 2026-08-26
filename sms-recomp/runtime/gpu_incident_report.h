#pragma once

#include <aurora/aurora.h>

#include <cstddef>

namespace sb::gpu_incident {

// Allocation-free writer used by both the device-loss callback and the post-mortem reporter.
// Output is always NUL-terminated when capacity is nonzero and truncates at capacity - 1.
class FixedBufferWriter {
  public:
    FixedBufferWriter(char* destination, std::size_t capacity) noexcept;

    void append(const char* format, ...) noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    char* destination_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
};

// Formats every field Aurora recorded for one queue submission. `reference` is normally the
// latest completed submit and makes the report name every recorded field that changed before the
// device loss. The returned size excludes the trailing NUL and may equal capacity - 1 when the
// output was truncated.
std::size_t format_submit_diagnostic(char* destination, std::size_t capacity,
                                     const AuroraGpuSubmitInfo& info,
                                     const AuroraGpuSubmitInfo* reference = nullptr) noexcept;

} // namespace sb::gpu_incident
