#include "gpu_incident_recorder.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    std::printf("  %s  %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition)
        ++g_failures;
}

std::filesystem::path fixture_directory() {
    return std::filesystem::path("scratch/tests/gpu_incident_recorder") /
           std::to_string(static_cast<unsigned long long>(::getpid()));
}

AuroraGpuSubmitInfo submit(std::uint64_t id) {
    AuroraGpuSubmitInfo info{};
    info.structSize = sizeof(info);
    info.version = 1;
    info.kind = AURORA_GPU_SUBMIT_FRAME;
    info.submitId = id;
    info.frameId = id + 100;
    info.frameIndex = static_cast<std::uint32_t>(id);
    info.passCount = 3;
    info.recordedPassCount = 3;
    info.drawCount = 179;
    info.operationCount = 184;
    info.vertexBytes = 4096;
    info.indexBytes = 2048;
    return info;
}

bool configure(const std::filesystem::path& path, std::uint64_t session) {
    return sb::gpu_incident::configure_file({
        .path = path,
        .sessionLabel = "recorder-control",
        .sessionId = session,
    });
}

void probe(AuroraGpuProbePhase phase, const AuroraGpuSubmitInfo& info, const char* message = "") {
    sbr_gpu_probe_callback(phase, &info, message, std::strlen(message), nullptr);
}

void clean_file(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
}

void test_abort_pending(const std::filesystem::path& directory) {
    std::puts("abort-pending crash survival");
    const auto file = directory / "abort_pending.flight";
    clean_file(file);
    constexpr std::uint64_t session = 0xA8017;
    const pid_t child = ::fork();
    check(child >= 0, "forked recorder control process");
    if (child == 0) {
        const rlimit noCore{0, 0};
        if (::setrlimit(RLIMIT_CORE, &noCore) != 0)
            _exit(91);
        if (!configure(file, session))
            _exit(90);
        const AuroraGpuSubmitInfo info = submit(17);
        probe(AURORA_GPU_PROBE_SUBMIT_BEGIN, info);
        std::abort(); // no shutdown and no RETURN: this is the crash-survival control
    }
    if (child < 0)
        return;
    int status = 0;
    check(::waitpid(child, &status, 0) == child, "waited for killed recorder process");
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
          "control process aborted between BEGIN and RETURN");
    const auto result =
        sb::gpu_incident::analyze_file(file, static_cast<std::uint64_t>(child), session);
    check(result.ok(), "aborted process left an analyzable flight file");
    check(result.submits.size() == 1 && result.submits.front().api_pending(),
          "aborted process preserves one API-pending submit");
}

void test_completed(const std::filesystem::path& directory) {
    std::puts("returned and completed submit");
    const auto file = directory / "completed.flight";
    clean_file(file);
    constexpr std::uint64_t session = 0xC011E7E;
    check(configure(file, session), "configured completed-submit fixture");
    const AuroraGpuSubmitInfo info = submit(23);
    probe(AURORA_GPU_PROBE_SUBMIT_BEGIN, info);
    probe(AURORA_GPU_PROBE_SUBMIT_RETURN, info);
    probe(AURORA_GPU_PROBE_SUBMIT_COMPLETE, info);
    sb::gpu_incident::shutdown();
    const auto result = sb::gpu_incident::analyze_file(file, ::getpid(), session);
    check(result.ok() && result.records.size() == 3, "all three queue phases survived");
    check(result.submits.size() == 1 && result.submits.front().returned &&
              result.submits.front().completed && !result.submits.front().api_pending() &&
              !result.submits.front().gpu_pending(),
          "RETURN and COMPLETE are distinct and both observed");
}

void test_corrupt_and_torn(const std::filesystem::path& directory) {
    std::puts("corrupt and torn records");
    const auto corruptFile = directory / "corrupt.flight";
    clean_file(corruptFile);
    check(configure(corruptFile, 0xC0FFEE), "configured corrupt-record fixture");
    const AuroraGpuSubmitInfo info = submit(31);
    probe(AURORA_GPU_PROBE_SUBMIT_BEGIN, info);
    sb::gpu_incident::shutdown();
    int fd = ::open(corruptFile.c_str(), O_RDWR | O_CLOEXEC);
    std::byte changed{0x5A};
    check(fd >= 0 && ::pwrite(fd, &changed, 1,
                              static_cast<off_t>(sb::gpu_incident::kFileHeaderBytes + 80)) == 1,
          "corrupted one committed payload byte");
    if (fd >= 0)
        ::close(fd);
    auto result = sb::gpu_incident::analyze_file(corruptFile);
    check(result.ok() && result.records.empty() && result.corruptRecordCount == 1,
          "checksum rejects a corrupt committed record");

    const auto tornFile = directory / "torn.flight";
    clean_file(tornFile);
    check(configure(tornFile, 0x7012), "configured torn-record fixture");
    probe(AURORA_GPU_PROBE_SUBMIT_BEGIN, info);
    sb::gpu_incident::shutdown();
    fd = ::open(tornFile.c_str(), O_RDWR | O_CLOEXEC);
    const std::uint64_t noCommit = 0;
    check(fd >= 0 && ::pwrite(fd, &noCommit, sizeof(noCommit),
                              static_cast<off_t>(sb::gpu_incident::kFileHeaderBytes +
                                                 sb::gpu_incident::kRecordCommitOffset)) ==
                         static_cast<ssize_t>(sizeof(noCommit)),
          "cleared the record commit marker");
    if (fd >= 0)
        ::close(fd);
    result = sb::gpu_incident::analyze_file(tornFile);
    check(result.ok() && result.records.empty() && result.corruptRecordCount == 1,
          "missing commit marker rejects a torn record");

    const auto shortFile = directory / "short.flight";
    clean_file(shortFile);
    fd = ::open(shortFile.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, S_IRUSR | S_IWUSR);
    const ssize_t written = fd >= 0 ? ::write(fd, "SBGPU", 5) : -1;
    if (fd >= 0)
        ::close(fd);
    check(written == 5, "wrote a truncated five-byte flight file");
    result = sb::gpu_incident::analyze_file(shortFile);
    check(result.status == sb::gpu_incident::AnalyzeStatus::InvalidHeader && !result.error.empty(),
          "a truncated file is an InvalidHeader WITH a named reason");
}

void test_wrap_and_stale_rejection(const std::filesystem::path& directory) {
    std::puts("ring wrap and stale identity rejection");
    const auto file = directory / "wrap.flight";
    clean_file(file);
    constexpr std::uint64_t session = 0x512;
    check(configure(file, session), "configured wrap fixture");
    for (std::uint64_t id = 1; id <= sb::gpu_incident::kRecordCapacity + 1; ++id) {
        const AuroraGpuSubmitInfo info = submit(id);
        probe(AURORA_GPU_PROBE_SUBMIT_BEGIN, info);
    }
    sb::gpu_incident::shutdown();
    const auto result = sb::gpu_incident::analyze_file(file, ::getpid(), session);
    check(result.ok() && result.records.size() == sb::gpu_incident::kRecordCapacity,
          "fixed ring retains exactly 512 records");
    check(!result.records.empty() && result.records.front().sequence == 2 &&
              result.records.back().sequence == sb::gpu_incident::kRecordCapacity + 1,
          "wrapped records remain chronological and newest");

    const auto staleProcess =
        sb::gpu_incident::analyze_file(file, static_cast<std::uint64_t>(::getpid()) + 1, session);
    check(staleProcess.status == sb::gpu_incident::AnalyzeStatus::StaleProcess,
          "wrong process cannot consume a valid stale flight file");
    const auto staleSession = sb::gpu_incident::analyze_file(file, ::getpid(), session + 1);
    check(staleSession.status == sb::gpu_incident::AnalyzeStatus::StaleSession,
          "wrong session cannot consume a valid stale flight file");
}

} // namespace

int main() {
    const auto directory = fixture_directory();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        std::fprintf(stderr, "cannot create test fixture directory %s: %s\n", directory.c_str(),
                     error.message().c_str());
        return 2;
    }

    test_abort_pending(directory);
    test_completed(directory);
    test_corrupt_and_torn(directory);
    test_wrap_and_stale_rejection(directory);
    sb::gpu_incident::shutdown();

    for (const auto& entry : std::filesystem::directory_iterator(directory))
        clean_file(entry.path());
    std::filesystem::remove(directory, error);

    std::printf("%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
