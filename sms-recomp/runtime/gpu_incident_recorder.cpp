#include "gpu_incident_recorder.h"
#include "gpu_incident_report.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <ranges>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <unistd.h>

namespace sb::gpu_incident {
namespace {

using Bytes = std::span<const std::byte>;

constexpr std::array<std::byte, 8> kHeaderMagic{
    std::byte{'S'}, std::byte{'B'}, std::byte{'G'}, std::byte{'P'},
    std::byte{'U'}, std::byte{'I'}, std::byte{'0'}, std::byte{'1'},
};
constexpr std::uint64_t kHeaderCommitMask = 0x484541444552434DULL;
constexpr std::uint64_t kRecordCommitMask = 0x5245434F5244434DULL;

constexpr std::size_t kHeaderChecksumOffset = 240;
constexpr std::size_t kHeaderCommitOffset = 248;
constexpr std::size_t kInfoOffset = 64;
constexpr std::size_t kInfoCapacity = 1536;
constexpr std::size_t kMessageOffset = kInfoOffset + kInfoCapacity;
constexpr std::size_t kMessageCapacity = AURORA_GPU_PROBE_MAX_MESSAGE;
constexpr std::size_t kRecordChecksumOffset = 2032;

static_assert(sizeof(AuroraGpuSubmitInfo) <= kInfoCapacity,
              "AuroraGpuSubmitInfo outgrew the persistent GPU incident format");
static_assert(kMessageOffset + kMessageCapacity == kRecordChecksumOffset);
static_assert(kRecordCommitOffset + sizeof(std::uint64_t) == kFileRecordBytes);

struct RecorderState {
    std::mutex mutex;
    int fd = -1;
    int reportFd = -1;
    bool reportWritten = false;
    bool healthy = false;
    bool errorReported = false;
    std::uint64_t nextSequence = 1;
    std::uint64_t processId = 0;
    std::uint64_t sessionId = 0;
    std::filesystem::path path;
    std::filesystem::path reportPath;
    AuroraGpuSubmitInfo lastBegan{};
    AuroraGpuSubmitInfo lastCompleted{};
    AuroraGpuSubmitInfo lastRealFrameBegan{};
    bool hasLastBegan = false;
    bool hasLastCompleted = false;
    bool hasLastRealFrameBegan = false;
    std::array<char, 256> lastError{};
};

RecorderState g_recorder;

std::uint64_t realtime_ns() noexcept {
    timespec time{};
    return clock_gettime(CLOCK_REALTIME, &time) == 0
               ? static_cast<std::uint64_t>(time.tv_sec) * 1'000'000'000ULL +
                     static_cast<std::uint64_t>(time.tv_nsec)
               : 0;
}

std::uint64_t monotonic_ns() noexcept {
    timespec time{};
    return clock_gettime(CLOCK_MONOTONIC, &time) == 0
               ? static_cast<std::uint64_t>(time.tv_sec) * 1'000'000'000ULL +
                     static_cast<std::uint64_t>(time.tv_nsec)
               : 0;
}

std::uint64_t current_thread_id() noexcept {
#ifdef __linux__
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#else
    return 0;
#endif
}

template <typename T, std::size_t Size>
void store(std::array<std::byte, Size>& bytes, std::size_t offset, const T& value) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template <typename T> T load(Bytes bytes, std::size_t offset) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::uint64_t checksum(Bytes bytes) noexcept {
    // FNV-1a is not a security primitive; it is a torn/partial-write detector. A committed record
    // is trusted only when both this checksum and the sequence-derived commit marker agree.
    std::uint64_t value = 1469598103934665603ULL;
    for (const std::byte byte : bytes) {
        value ^= static_cast<std::uint8_t>(byte);
        value *= 1099511628211ULL;
    }
    return value;
}

bool all_zero(Bytes bytes) noexcept {
    return std::ranges::all_of(bytes, [](std::byte value) { return value == std::byte{0}; });
}

bool pwrite_all(int fd, Bytes bytes, off_t offset) noexcept {
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t count = ::pwrite(fd, bytes.data() + written, bytes.size() - written,
                                       offset + static_cast<off_t>(written));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        written += static_cast<std::size_t>(count);
    }
    return true;
}

void write_best_effort(int fd, Bytes bytes) noexcept {
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t count = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return;
        written += static_cast<std::size_t>(count);
    }
}

void set_error_locked(const char* operation) noexcept {
    const int error = errno;
    std::snprintf(g_recorder.lastError.data(), g_recorder.lastError.size(), "%s: %s", operation,
                  std::strerror(error));
    g_recorder.healthy = false;
    if (!g_recorder.errorReported) {
        g_recorder.errorReported = true;
        // LOGGER-EXEMPT: this recorder sits below the configurable logger and reports only the
        // first failure of the crash instrument itself. Silence would mislabel missing evidence.
        std::fprintf(stderr, "[gpu-incident] RECORDER FAILURE: %s\n", g_recorder.lastError.data());
        std::fflush(stderr);
    }
}

std::string clean_label(std::string_view label) {
    std::string result;
    result.reserve(std::min<std::size_t>(label.size(), 48));
    for (const char character : label.substr(0, 48)) {
        const bool safe =
            (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_';
        result.push_back(safe ? character : '_');
    }
    return result.empty() ? "session" : result;
}

void close_locked() noexcept {
    if (g_recorder.fd >= 0)
        ::close(g_recorder.fd);
    if (g_recorder.reportFd >= 0)
        ::close(g_recorder.reportFd);
    if (!g_recorder.reportWritten && !g_recorder.reportPath.empty())
        ::unlink(g_recorder.reportPath.c_str());
    g_recorder.fd = -1;
    g_recorder.reportFd = -1;
    g_recorder.reportWritten = false;
    g_recorder.healthy = false;
    g_recorder.nextSequence = 1;
    g_recorder.processId = 0;
    g_recorder.sessionId = 0;
    g_recorder.path.clear();
    g_recorder.reportPath.clear();
    g_recorder.lastBegan = {};
    g_recorder.lastCompleted = {};
    g_recorder.lastRealFrameBegan = {};
    g_recorder.hasLastBegan = false;
    g_recorder.hasLastCompleted = false;
    g_recorder.hasLastRealFrameBegan = false;
}

bool write_header_locked(std::string_view sessionLabel) noexcept {
    std::array<std::byte, kFileHeaderBytes> header{};
    std::copy(kHeaderMagic.begin(), kHeaderMagic.end(), header.begin());
    store(header, 8, kFormatVersion);
    store(header, 12, static_cast<std::uint32_t>(kFileHeaderBytes));
    store(header, 16, static_cast<std::uint32_t>(kFileRecordBytes));
    store(header, 20, static_cast<std::uint32_t>(kRecordCapacity));
    store(header, 24, g_recorder.processId);
    store(header, 32, g_recorder.sessionId);
    store(header, 40, realtime_ns());
    store(header, 48, monotonic_ns());
    const std::size_t labelSize = std::min<std::size_t>(sessionLabel.size(), 63);
    std::memcpy(header.data() + 56, sessionLabel.data(), labelSize);
    const std::uint64_t digest = checksum(Bytes{header}.first(kHeaderChecksumOffset));
    store(header, kHeaderChecksumOffset, digest);
    store(header, kHeaderCommitOffset, digest ^ kHeaderCommitMask);
    if (!pwrite_all(g_recorder.fd, Bytes{header}, 0)) {
        set_error_locked("write GPU incident header");
        return false;
    }
    if (::fsync(g_recorder.fd) != 0) {
        set_error_locked("synchronize GPU incident header");
        return false;
    }
    return true;
}

void write_probe_locked(AuroraGpuProbePhase phase, const AuroraGpuSubmitInfo* info,
                        const char* message, std::size_t messageLength) noexcept {
    if (!g_recorder.healthy || g_recorder.fd < 0)
        return;

    const std::uint64_t sequence = g_recorder.nextSequence++;
    const std::size_t slot = static_cast<std::size_t>((sequence - 1) % kRecordCapacity);
    const off_t offset = static_cast<off_t>(kFileHeaderBytes + slot * kFileRecordBytes);

    // Invalidate the old wrapped record first. If this process dies during the following pwrite,
    // the analyzer sees a corrupt/torn slot, never an older record masquerading as the latest one.
    const std::uint64_t invalidCommit = 0;
    if (!pwrite_all(
            g_recorder.fd,
            Bytes{reinterpret_cast<const std::byte*>(&invalidCommit), sizeof(invalidCommit)},
            offset + static_cast<off_t>(kRecordCommitOffset))) {
        set_error_locked("invalidate GPU incident record");
        return;
    }

    std::array<std::byte, kFileRecordBytes> record{};
    store(record, 0, sequence);
    store(record, 8, realtime_ns());
    store(record, 16, monotonic_ns());
    store(record, 24, g_recorder.processId);
    store(record, 32, current_thread_id());
    store(record, 40, g_recorder.sessionId);
    store(record, 48, static_cast<std::uint32_t>(phase));

    std::uint32_t infoSize = 0;
    if (info != nullptr) {
        const std::size_t declared =
            info->structSize == 0 ? sizeof(*info) : static_cast<std::size_t>(info->structSize);
        infoSize = static_cast<std::uint32_t>(std::min({declared, sizeof(*info), kInfoCapacity}));
        std::memcpy(record.data() + kInfoOffset, info, infoSize);
    }
    store(record, 52, infoSize);

    const std::uint32_t storedMessageLength = static_cast<std::uint32_t>(
        message == nullptr ? 0 : std::min(messageLength, kMessageCapacity));
    store(record, 56, storedMessageLength);
    if (storedMessageLength != 0)
        std::memcpy(record.data() + kMessageOffset, message, storedMessageLength);

    const std::uint64_t digest = checksum(Bytes{record}.first(kRecordChecksumOffset));
    store(record, kRecordChecksumOffset, digest);
    store(record, kRecordCommitOffset, sequence ^ digest ^ kRecordCommitMask);
    if (!pwrite_all(g_recorder.fd, Bytes{record}, offset))
        set_error_locked("write GPU incident record");
}

void write_incident_report_locked(AuroraGpuProbePhase phase, const AuroraGpuSubmitInfo* pending,
                                  const char* message, std::size_t messageLength) noexcept {
    const bool uncapturedError = phase == AURORA_GPU_PROBE_UNCAPTURED_ERROR;
    std::array<char, 32 * 1024> report{};
    FixedBufferWriter writer(report.data(), report.size());
    writer.append("SUNBRIGHT GPU %s\n", uncapturedError ? "UNCAPTURED ERROR" : "DEVICE LOSS");
    writer.append("pid=%llu session=%016llx real_ns=%llu mono_ns=%llu\n",
                  static_cast<unsigned long long>(g_recorder.processId),
                  static_cast<unsigned long long>(g_recorder.sessionId),
                  static_cast<unsigned long long>(realtime_ns()),
                  static_cast<unsigned long long>(monotonic_ns()));
    writer.append("flight_file=%s\n", g_recorder.path.c_str());
    if (message != nullptr && messageLength != 0) {
        const int safeLength =
            static_cast<int>(std::min<std::size_t>(messageLength, kMessageCapacity));
        writer.append("driver_message=%.*s\n", safeLength, message);
    } else {
        writer.append("driver_message=(callback supplied no message)\n");
    }
    if (pending != nullptr) {
        writer.append("latest submission at %s (temporal context only):\n",
                      uncapturedError ? "uncaptured error" : "device loss");
        std::array<char, 24 * 1024> submitReport{};
        const AuroraGpuSubmitInfo* replaySource = nullptr;
        if (pending->replayEmission != 0 && has_replay_source_lineage(*pending) &&
            g_recorder.hasLastRealFrameBegan &&
            has_replay_source_lineage(g_recorder.lastRealFrameBegan) &&
            g_recorder.lastRealFrameBegan.frameId == pending->replaySourceFrameId) {
            replaySource = &g_recorder.lastRealFrameBegan;
        }
        const std::size_t submitReportSize = format_submit_diagnostic(
            submitReport.data(), submitReport.size(), *pending,
            g_recorder.hasLastCompleted ? &g_recorder.lastCompleted : nullptr, replaySource);
        writer.append("%.*s", static_cast<int>(submitReportSize), submitReport.data());
    } else {
        writer.append("latest submission at %s: unavailable (no SUBMIT_BEGIN recorded)\n",
                      uncapturedError ? "uncaptured error" : "device loss");
    }
    if (uncapturedError) {
        writer.append(
            "cause: Dawn delivered an uncaptured WebGPU error before Aurora's fatal abort. The "
            "submission above is only the latest queue call observed by this process; temporal "
            "association is not proof that it caused the error. The bounded driver_message "
            "preserves Dawn's error type and text.\n");
    } else {
        writer.append(
            "cause: the GPU API reported device loss. The submission above is the latest queue "
            "call observed by this process; this report does not claim that one pending submit "
            "was the faulting command stream. DEVICE_LOST and completion callbacks can lag the "
            "kernel's first illegal-CS, GPUVM, or ring-timeout event, so later submissions may "
            "be aftermath. Pair this file with the first kernel event timestamp.\n");
    }

    bool reportWritten = g_recorder.reportFd >= 0;
    if (reportWritten && ::ftruncate(g_recorder.reportFd, 0) != 0)
        reportWritten = false;
    if (reportWritten &&
        !pwrite_all(g_recorder.reportFd,
                    Bytes{reinterpret_cast<const std::byte*>(report.data()), writer.size()}, 0)) {
        reportWritten = false;
    }
    if (reportWritten && ::fdatasync(g_recorder.reportFd) != 0)
        reportWritten = false;
    if (reportWritten)
        g_recorder.reportWritten = true;
    if (::fdatasync(g_recorder.fd) != 0)
        set_error_locked("synchronize GPU incident flight file after callback error");
    if (!reportWritten)
        set_error_locked("write durable GPU callback-error report");

    // LOGGER-EXEMPT: a GPU callback error may abort before the configurable logger drains. Keep
    // stderr concise; the complete fixed diagnostic is already synchronized in the named sidecar.
    std::array<char, 1024> notice{};
    FixedBufferWriter noticeWriter(notice.data(), notice.size());
    noticeWriter.append("[gpu-incident] %s at submit=%llu; durable report: %s",
                        uncapturedError ? "UNCAPTURED ERROR" : "DEVICE LOST",
                        static_cast<unsigned long long>(pending == nullptr ? 0 : pending->submitId),
                        g_recorder.reportPath.c_str());
    if (message != nullptr && messageLength != 0) {
        noticeWriter.append("; driver: %.*s",
                            static_cast<int>(std::min<std::size_t>(messageLength, 256)), message);
    }
    noticeWriter.append("\n");
    write_best_effort(STDERR_FILENO, Bytes{reinterpret_cast<const std::byte*>(notice.data()),
                                           noticeWriter.size()});
}

bool valid_header(Bytes header, std::string& error) {
    if (header.size() != kFileHeaderBytes ||
        !std::equal(kHeaderMagic.begin(), kHeaderMagic.end(), header.begin())) {
        error = "GPU incident header magic is missing";
        return false;
    }
    if (load<std::uint32_t>(header, 8) != kFormatVersion ||
        load<std::uint32_t>(header, 12) != kFileHeaderBytes ||
        load<std::uint32_t>(header, 16) != kFileRecordBytes ||
        load<std::uint32_t>(header, 20) != kRecordCapacity) {
        error = "GPU incident file format does not match this analyzer";
        return false;
    }
    const std::uint64_t expected = checksum(header.first(kHeaderChecksumOffset));
    if (load<std::uint64_t>(header, kHeaderChecksumOffset) != expected ||
        load<std::uint64_t>(header, kHeaderCommitOffset) != (expected ^ kHeaderCommitMask)) {
        error = "GPU incident header is torn or corrupt";
        return false;
    }
    return true;
}

enum class RecordValidation : std::uint8_t {
    Valid,
    InvalidBounds,
    Corrupt,
};

RecordValidation validate_record(Bytes record) noexcept {
    const std::uint64_t sequence = load<std::uint64_t>(record, 0);
    if (sequence == 0)
        return RecordValidation::Corrupt;
    if (load<std::uint32_t>(record, 48) >
            static_cast<std::uint32_t>(AURORA_GPU_PROBE_UNCAPTURED_ERROR) ||
        load<std::uint32_t>(record, 52) > sizeof(AuroraGpuSubmitInfo) ||
        load<std::uint32_t>(record, 56) > kMessageCapacity) {
        return RecordValidation::InvalidBounds;
    }
    const std::uint64_t expected = checksum(record.first(kRecordChecksumOffset));
    return load<std::uint64_t>(record, kRecordChecksumOffset) == expected &&
                   load<std::uint64_t>(record, kRecordCommitOffset) ==
                       (sequence ^ expected ^ kRecordCommitMask)
               ? RecordValidation::Valid
               : RecordValidation::Corrupt;
}

} // namespace

static void record_probe(AuroraGpuProbePhase phase, const AuroraGpuSubmitInfo* info,
                         const char* message, std::size_t messageLength) noexcept {
    std::lock_guard lock(g_recorder.mutex);
    const AuroraGpuSubmitInfo* persistedInfo = info;
    if (phase == AURORA_GPU_PROBE_SUBMIT_BEGIN && info != nullptr) {
        g_recorder.lastBegan = *info;
        g_recorder.hasLastBegan = true;
        if (info->kind == AURORA_GPU_SUBMIT_FRAME && info->replayEmission == 0 &&
            has_replay_source_lineage(*info)) {
            g_recorder.lastRealFrameBegan = *info;
            g_recorder.hasLastRealFrameBegan = true;
        }
    } else if (phase == AURORA_GPU_PROBE_SUBMIT_COMPLETE && info != nullptr &&
               info->status == AURORA_GPU_SUBMIT_STATUS_SUCCESS) {
        g_recorder.lastCompleted = *info;
        g_recorder.hasLastCompleted = true;
    } else if ((phase == AURORA_GPU_PROBE_DEVICE_LOST ||
                phase == AURORA_GPU_PROBE_UNCAPTURED_ERROR) &&
               (persistedInfo == nullptr || persistedInfo->submitId == 0) &&
               g_recorder.hasLastBegan) {
        // Dawn's error callbacks have no queue submission payload. Associate the incident record
        // with the latest BEGIN so the binary record and durable text retain temporal context.
        // This is not causal attribution: callbacks may be delayed relative to the originating
        // command stream or kernel event.
        persistedInfo = &g_recorder.lastBegan;
    }
    write_probe_locked(phase, persistedInfo, message, messageLength);
    if (phase == AURORA_GPU_PROBE_DEVICE_LOST || phase == AURORA_GPU_PROBE_UNCAPTURED_ERROR) {
        write_incident_report_locked(phase, persistedInfo, message, messageLength);
    }
}

bool configure_file(const ConfigureOptions& options) noexcept {
    std::lock_guard lock(g_recorder.mutex);
    close_locked();
    g_recorder.lastError.fill('\0');
    g_recorder.errorReported = false;

    std::error_code filesystemError;
    const auto parent = options.path.parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, filesystemError);
    if (filesystemError) {
        errno = filesystemError.value();
        set_error_locked("create GPU incident directory");
        return false;
    }

    g_recorder.fd =
        ::open(options.path.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (g_recorder.fd < 0) {
        set_error_locked("open GPU incident file");
        return false;
    }
    const off_t fileSize =
        static_cast<off_t>(kFileHeaderBytes + kRecordCapacity * kFileRecordBytes);
    if (::ftruncate(g_recorder.fd, fileSize) != 0) {
        set_error_locked("size GPU incident file");
        close_locked();
        return false;
    }
    g_recorder.reportPath = options.path;
    g_recorder.reportPath += ".report.txt";
    g_recorder.reportFd = ::open(g_recorder.reportPath.c_str(),
                                 O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (g_recorder.reportFd < 0) {
        set_error_locked("open GPU callback-error report");
        close_locked();
        return false;
    }

    g_recorder.processId =
        options.processId == 0 ? static_cast<std::uint64_t>(::getpid()) : options.processId;
    g_recorder.sessionId = options.sessionId == 0
                               ? realtime_ns() ^ (monotonic_ns() << 1U) ^ g_recorder.processId
                               : options.sessionId;
    g_recorder.path = options.path;
    g_recorder.healthy = true;
    if (!write_header_locked(clean_label(options.sessionLabel))) {
        close_locked();
        return false;
    }
    return true;
}

bool configure_directory(const std::filesystem::path& directory,
                         std::string_view sessionLabel) noexcept {
    const std::uint64_t generatedSession =
        realtime_ns() ^ (monotonic_ns() << 1U) ^ static_cast<std::uint64_t>(::getpid());
    char filename[160];
    const std::string label = clean_label(sessionLabel);
    std::snprintf(filename, sizeof(filename), "session_%llu_%016llx_%s.flight",
                  static_cast<unsigned long long>(::getpid()),
                  static_cast<unsigned long long>(generatedSession), label.c_str());
    return configure_file(ConfigureOptions{
        .path = directory / filename,
        .sessionLabel = label,
        .sessionId = generatedSession,
    });
}

void shutdown() noexcept {
    std::lock_guard lock(g_recorder.mutex);
    close_locked();
}

bool healthy() noexcept {
    std::lock_guard lock(g_recorder.mutex);
    return g_recorder.healthy;
}

std::uint64_t session_id() noexcept {
    std::lock_guard lock(g_recorder.mutex);
    return g_recorder.sessionId;
}

std::filesystem::path path() {
    std::lock_guard lock(g_recorder.mutex);
    return g_recorder.path;
}

std::filesystem::path report_path() {
    std::lock_guard lock(g_recorder.mutex);
    return g_recorder.reportPath;
}

const char* last_error() noexcept {
    std::lock_guard lock(g_recorder.mutex);
    return g_recorder.lastError.data();
}

Analysis analyze_file(const std::filesystem::path& filePath,
                      std::optional<std::uint64_t> expectedProcessId,
                      std::optional<std::uint64_t> expectedSessionId) {
    Analysis result;
    const int fd = ::open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        result.error = "cannot open GPU incident file: " + std::string(std::strerror(errno));
        return result;
    }

    std::array<std::byte, kFileHeaderBytes> header{};
    const ssize_t headerRead = ::pread(fd, header.data(), header.size(), 0);
    if (headerRead != static_cast<ssize_t>(header.size()) ||
        !valid_header(Bytes{header}, result.error)) {
        result.status = AnalyzeStatus::InvalidHeader;
        if (result.error.empty())
            result.error = "file does not contain a complete GPU incident header (" +
                           std::to_string(headerRead) + " of " + std::to_string(header.size()) +
                           " bytes readable)";
        ::close(fd);
        return result;
    }

    result.processId = load<std::uint64_t>(Bytes{header}, 24);
    result.sessionId = load<std::uint64_t>(Bytes{header}, 32);
    const char* label = reinterpret_cast<const char*>(header.data() + 56);
    result.sessionLabel.assign(label, strnlen(label, 64));
    if (expectedProcessId && *expectedProcessId != result.processId) {
        result.status = AnalyzeStatus::StaleProcess;
        result.error = "GPU incident file belongs to another process";
        ::close(fd);
        return result;
    }
    if (expectedSessionId && *expectedSessionId != result.sessionId) {
        result.status = AnalyzeStatus::StaleSession;
        result.error = "GPU incident file belongs to another session";
        ::close(fd);
        return result;
    }

    std::array<std::byte, kFileRecordBytes> raw{};
    for (std::size_t slot = 0; slot < kRecordCapacity; ++slot) {
        const off_t offset = static_cast<off_t>(kFileHeaderBytes + slot * kFileRecordBytes);
        const ssize_t count = ::pread(fd, raw.data(), raw.size(), offset);
        if (count != static_cast<ssize_t>(raw.size())) {
            result.status = AnalyzeStatus::IoError;
            result.error = "GPU incident file ended inside its fixed record ring";
            ::close(fd);
            return result;
        }
        if (all_zero(Bytes{raw}))
            continue;
        const RecordValidation validation = validate_record(Bytes{raw});
        if (validation != RecordValidation::Valid) {
            ++result.corruptRecordCount;
            if (validation == RecordValidation::InvalidBounds)
                ++result.invalidBoundsRecordCount;
            continue;
        }
        if (load<std::uint64_t>(Bytes{raw}, 24) != result.processId ||
            load<std::uint64_t>(Bytes{raw}, 40) != result.sessionId) {
            ++result.corruptRecordCount;
            continue;
        }

        Record record;
        record.sequence = load<std::uint64_t>(Bytes{raw}, 0);
        record.realtimeNs = load<std::uint64_t>(Bytes{raw}, 8);
        record.monotonicNs = load<std::uint64_t>(Bytes{raw}, 16);
        record.processId = load<std::uint64_t>(Bytes{raw}, 24);
        record.threadId = load<std::uint64_t>(Bytes{raw}, 32);
        record.sessionId = load<std::uint64_t>(Bytes{raw}, 40);
        record.phase = static_cast<AuroraGpuProbePhase>(load<std::uint32_t>(Bytes{raw}, 48));
        const std::uint32_t infoSize = load<std::uint32_t>(Bytes{raw}, 52);
        if (infoSize != 0) {
            std::memcpy(&record.info, raw.data() + kInfoOffset, infoSize);
            // The persisted byte count is authoritative. A corrupt or future producer must not
            // make the formatter read tail fields that were not actually present in this record.
            record.info.structSize = std::min<std::uint32_t>(record.info.structSize, infoSize);
        }
        const std::uint32_t messageSize = load<std::uint32_t>(Bytes{raw}, 56);
        record.message.assign(reinterpret_cast<const char*>(raw.data() + kMessageOffset),
                              messageSize);
        result.records.push_back(std::move(record));
    }
    ::close(fd);

    std::ranges::sort(result.records, {}, &Record::sequence);
    std::map<std::uint64_t, SubmitState> submits;
    for (const Record& record : result.records) {
        if (record.info.submitId == 0)
            continue;
        SubmitState& state = submits[record.info.submitId];
        state.submitId = record.info.submitId;
        switch (record.phase) {
        case AURORA_GPU_PROBE_SUBMIT_BEGIN:
            state.began = true;
            state.beginSequence = record.sequence;
            break;
        case AURORA_GPU_PROBE_SUBMIT_RETURN:
            state.returned = true;
            state.returnSequence = record.sequence;
            break;
        case AURORA_GPU_PROBE_SUBMIT_COMPLETE:
            state.completionCallbackObserved = true;
            state.completed = record.info.status == AURORA_GPU_SUBMIT_STATUS_SUCCESS;
            state.completeSequence = record.sequence;
            break;
        case AURORA_GPU_PROBE_DEVICE_LOST:
        case AURORA_GPU_PROBE_UNCAPTURED_ERROR:
            break;
        }
    }
    for (auto& [id, state] : submits)
        result.submits.push_back(state);
    std::ranges::sort(result.submits, {}, &SubmitState::beginSequence);
    result.status = AnalyzeStatus::Ok;
    return result;
}

} // namespace sb::gpu_incident

extern "C" bool sbr_gpu_incident_configure(const char* directory,
                                           const char* sessionName) noexcept {
    if (directory == nullptr || directory[0] == '\0')
        return false;
    return sb::gpu_incident::configure_directory(
        directory, sessionName == nullptr ? std::string_view{"sunbright"} : sessionName);
}

extern "C" void sbr_gpu_incident_shutdown() noexcept {
    sb::gpu_incident::shutdown();
}

extern "C" bool sbr_gpu_incident_healthy() noexcept {
    return sb::gpu_incident::healthy();
}

extern "C" std::uint64_t sbr_gpu_incident_session_id() noexcept {
    return sb::gpu_incident::session_id();
}

extern "C" const char* sbr_gpu_incident_last_error() noexcept {
    return sb::gpu_incident::last_error();
}

extern "C" void sbr_gpu_probe_callback(AuroraGpuProbePhase phase, const AuroraGpuSubmitInfo* info,
                                       const char* message, std::size_t messageLen,
                                       void* /*user*/) noexcept {
    sb::gpu_incident::record_probe(phase, info, message, messageLen);
}
