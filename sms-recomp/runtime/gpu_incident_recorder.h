#pragma once

#include <aurora/aurora.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Host-owned, process-crash-surviving GPU submit recorder.
//
// Aurora reports every queue boundary through one typed callback. The recorder writes fixed-size
// records into a bounded pwrite ring before returning to Aurora, so a later SIGABRT cannot erase
// the last SUBMIT_BEGIN. This is deliberately not a watcher, helper process, or journal reader.
// Kernel evidence remains kernel-owned; this file records exactly what this process submitted.

namespace sb::gpu_incident {

inline constexpr std::uint32_t kFormatVersion = 1;
inline constexpr std::size_t kRecordCapacity = 512;
inline constexpr std::size_t kFileHeaderBytes = 256;
inline constexpr std::size_t kFileRecordBytes = 2048;
inline constexpr std::size_t kRecordCommitOffset = 2040;

struct ConfigureOptions {
    std::filesystem::path path;
    std::string sessionLabel;
    // Zero selects the runtime value. Nonzero values make deterministic fixtures possible without
    // giving tests a second record writer.
    std::uint64_t sessionId = 0;
    std::uint64_t processId = 0;
};

enum class AnalyzeStatus {
    Ok,
    IoError,
    InvalidHeader,
    StaleProcess,
    StaleSession,
};

struct Record {
    std::uint64_t sequence = 0;
    std::uint64_t realtimeNs = 0;
    std::uint64_t monotonicNs = 0;
    std::uint64_t processId = 0;
    std::uint64_t threadId = 0;
    std::uint64_t sessionId = 0;
    AuroraGpuProbePhase phase = AURORA_GPU_PROBE_SUBMIT_BEGIN;
    AuroraGpuSubmitInfo info{};
    std::string message;
};

struct SubmitState {
    std::uint64_t submitId = 0;
    std::uint64_t beginSequence = 0;
    std::uint64_t returnSequence = 0;
    std::uint64_t completeSequence = 0;
    bool began = false;
    bool returned = false;
    bool completed = false;

    [[nodiscard]] bool api_pending() const noexcept { return began && !returned; }
    [[nodiscard]] bool gpu_pending() const noexcept { return began && returned && !completed; }
};

struct Analysis {
    AnalyzeStatus status = AnalyzeStatus::IoError;
    std::string error;
    std::uint64_t processId = 0;
    std::uint64_t sessionId = 0;
    std::string sessionLabel;
    std::size_t corruptRecordCount = 0;
    std::vector<Record> records;
    std::vector<SubmitState> submits;

    [[nodiscard]] bool ok() const noexcept { return status == AnalyzeStatus::Ok; }
};

// Opens one explicit file and installs it as the process-global recorder used by the C callback.
// The header is written and synchronized before this returns. Call before Aurora initialization.
bool configure_file(const ConfigureOptions& options) noexcept;

// Creates a unique session file below directory and installs it as the global recorder.
bool configure_directory(const std::filesystem::path& directory,
                         std::string_view sessionLabel) noexcept;

void shutdown() noexcept;
[[nodiscard]] bool healthy() noexcept;
[[nodiscard]] std::uint64_t session_id() noexcept;
[[nodiscard]] std::filesystem::path path();
[[nodiscard]] const char* last_error() noexcept;

// Pure post-mortem seam. Expected values make a stale process/session an explicit result instead
// of allowing an old valid file to impersonate the current crash.
Analysis analyze_file(const std::filesystem::path& path,
                      std::optional<std::uint64_t> expectedProcessId = std::nullopt,
                      std::optional<std::uint64_t> expectedSessionId = std::nullopt);

} // namespace sb::gpu_incident

extern "C" {

// C lifecycle used by the host composition root.
bool sbr_gpu_incident_configure(const char* directory, const char* sessionName) noexcept;
void sbr_gpu_incident_shutdown() noexcept;
bool sbr_gpu_incident_healthy() noexcept;
std::uint64_t sbr_gpu_incident_session_id() noexcept;
const char* sbr_gpu_incident_last_error() noexcept;

// Exact AuroraConfig::gpuProbeCallback signature. `SUBMIT_RETURN` means only that the host queue
// API returned; `SUBMIT_COMPLETE` is the independent GPU-completion watermark.
void sbr_gpu_probe_callback(AuroraGpuProbePhase phase, const AuroraGpuSubmitInfo* info,
                            const char* message, std::size_t messageLen, void* user) noexcept;
}
