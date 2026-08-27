#pragma once

#include <aurora/aurora.h>

#include <cstddef>

namespace sb::gpu_incident {

// Allocation-free writer used by GPU callback-error sidecars and the post-mortem reporter.
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

// The persisted byte count, producer version and nonzero source frame jointly gate lineage.
// This keeps historical, truncated and ordinary zero-lineage v3 records from becoming evidence.
[[nodiscard]] bool has_replay_source_lineage(const AuroraGpuSubmitInfo& info) noexcept;

// Formats every field Aurora recorded for one queue submission. `completedReference` is normally
// the latest successfully completed submit and names ordinary workload changes.
// `replaySourceReference` is a distinct, explicitly resolved real-emission BEGIN whose v3 lineage
// matches info.replaySourceFrameId; a completed baseline must never be substituted for it. The
// returned size excludes the trailing NUL and may equal capacity - 1 when output was truncated.
std::size_t
format_submit_diagnostic(char* destination, std::size_t capacity, const AuroraGpuSubmitInfo& info,
                         const AuroraGpuSubmitInfo* completedReference = nullptr,
                         const AuroraGpuSubmitInfo* replaySourceReference = nullptr) noexcept;

} // namespace sb::gpu_incident
