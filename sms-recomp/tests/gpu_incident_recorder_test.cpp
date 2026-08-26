#include "gpu_incident_recorder.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

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
    info.version = 2;
    info.kind = AURORA_GPU_SUBMIT_FRAME;
    info.submitId = id;
    info.frameId = id + 100;
    info.frameIndex = static_cast<std::uint32_t>(id);
    info.passCount = 3;
    info.recordedPassCount = 3;
    info.drawCount = 179;
    info.operationCount = 184;
    info.vertexBytes = 4096;
    info.uniformBytes = 1024;
    info.indexBytes = 2048;
    info.storageBytes = 8192;
    info.textureUploadBytes = 512;
    info.cachedTextureObjects = 41;
    info.cachedTlutObjects = 7;
    info.cachedCopyTextures = 3;
    info.cachedBindGroups = 29;
    info.persistentStorageEntries = 11;
    info.persistentStorageBytes = 65536;
    info.commandHash = 0xC011A4D000000000ULL + id;
    info.pipelineHash = 0xA1A311E000000000ULL + id;
    info.passes[0] = {
        .labelHash = 0x1ABE1000ULL + id,
        .commandHash = 0xC0A4A000ULL + id,
        .pipelineHash = 0xA1A31000ULL + id,
        .commandCount = 80,
        .drawCount = 75,
        .targetWidth = 1280,
        .targetHeight = 960,
        .flags = 1U | (1U << 3U) | (1U << 4U) | (4U << 8U),
    };
    info.recordedDrawCount = 1;
    info.firstRecordedDraw = 178;
    info.readbackQueuedThisSubmit = 1;
    info.readbackBytesThisSubmit = 1280 * 960 * 4;
    info.readbackMapsPending = 2;
    info.readbackMapsCompleted = 8;
    info.draws[0] = {
        .drawHash = 0xD2A0000000000000ULL + id,
        .pipelineId = 0xA1A3000000000000ULL + id,
        .tag = 0x47584649ULL,
        .passIndex = 0,
        .commandIndex = 79,
        .drawIndex = 178,
        .vertexOffset = 2048,
        .vertexBytes = 512,
        .indexOffset = 1024,
        .indexBytes = 96,
        .uniformOffset = 256,
        .uniformBytes = 1024,
        .shaderType = static_cast<std::uint8_t>(AURORA_GPU_DRAW_GX),
        .population = 4,
        .flags = static_cast<std::uint8_t>(AURORA_GPU_DRAW_FLAG_DEFORMING |
                                           AURORA_GPU_DRAW_FLAG_CAMERA_TEX_MATRIX),
    };
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

void complete(AuroraGpuSubmitInfo info,
              AuroraGpuSubmitStatus status = AURORA_GPU_SUBMIT_STATUS_SUCCESS) {
    info.status = status;
    probe(AURORA_GPU_PROBE_SUBMIT_COMPLETE, info);
}

void device_lost(const char* message) {
    sbr_gpu_probe_callback(AURORA_GPU_PROBE_DEVICE_LOST, nullptr, message, std::strlen(message),
                           nullptr);
}

void clean_file(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
}

std::pair<int, std::string> run_flight_dump(const std::filesystem::path& file,
                                            std::uint64_t kernelRealtimeNs) {
    int descriptors[2]{};
    if (::pipe(descriptors) != 0)
        return {-1, {}};
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(descriptors[0]);
        ::dup2(descriptors[1], STDOUT_FILENO);
        ::dup2(descriptors[1], STDERR_FILENO);
        ::close(descriptors[1]);
        const std::string timestamp = std::to_string(kernelRealtimeNs);
        ::execl(SBR_GPU_FLIGHT_DUMP_PATH, SBR_GPU_FLIGHT_DUMP_PATH, file.c_str(),
                "--kernel-real-ns", timestamp.c_str(), "--tail", "0", nullptr);
        _exit(127);
    }
    ::close(descriptors[1]);
    std::string output;
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t count = ::read(descriptors[0], buffer.data(), buffer.size());
        if (count <= 0)
            break;
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    ::close(descriptors[0]);
    int status = 0;
    if (child < 0 || ::waitpid(child, &status, 0) != child || !WIFEXITED(status))
        return {-1, std::move(output)};
    return {WEXITSTATUS(status), std::move(output)};
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
    complete(info);
    sb::gpu_incident::shutdown();
    const auto result = sb::gpu_incident::analyze_file(file, ::getpid(), session);
    check(result.ok() && result.records.size() == 3, "all three queue phases survived");
    check(result.submits.size() == 1 && result.submits.front().returned &&
              result.submits.front().completed && !result.submits.front().api_pending() &&
              !result.submits.front().completion_callback_pending(),
          "RETURN and COMPLETE are distinct and both observed");
}

void test_device_lost_report(const std::filesystem::path& directory) {
    std::puts("device-loss association and durable report");
    const auto file = directory / "device_lost.flight";
    clean_file(file);
    clean_file(file.string() + ".report.txt");
    constexpr std::uint64_t session = 0xDE71CE1057ULL;
    check(configure(file, session), "configured device-loss fixture");
    const AuroraGpuSubmitInfo completed = submit(40);
    probe(AURORA_GPU_PROBE_SUBMIT_BEGIN, completed);
    probe(AURORA_GPU_PROBE_SUBMIT_RETURN, completed);
    complete(completed);
    AuroraGpuSubmitInfo pending = submit(41);
    pending.drawCount = 213;
    pending.passes[0].drawCount = 109;
    probe(AURORA_GPU_PROBE_SUBMIT_BEGIN, pending);
    const auto reportFile = sb::gpu_incident::report_path();
    device_lost("vkQueueSubmit failed with VK_ERROR_DEVICE_LOST");
    sb::gpu_incident::shutdown();

    const auto result = sb::gpu_incident::analyze_file(file, ::getpid(), session);
    check(result.ok() && result.records.size() == 5,
          "device loss and its associated submit survived in the flight ring");
    const auto& loss = result.records.back();
    check(loss.phase == AURORA_GPU_PROBE_DEVICE_LOST && loss.info.submitId == pending.submitId &&
              loss.info.drawCount == pending.drawCount,
          "null-payload DEVICE_LOST is associated with the latest SUBMIT_BEGIN");

    std::ifstream stream(reportFile);
    const std::string report{std::istreambuf_iterator<char>{stream},
                             std::istreambuf_iterator<char>{}};
    check(!report.empty(), "device-loss sidecar was synchronized and readable");
    check(report.find("SUNBRIGHT GPU DEVICE LOSS") != std::string::npos &&
              report.find("vkQueueSubmit failed with VK_ERROR_DEVICE_LOST") != std::string::npos,
          "durable report names the driver failure location");
    check(report.find("submit=41") != std::string::npos &&
              report.find("draws=213") != std::string::npos &&
              report.find("pass[0]") != std::string::npos &&
              report.find("pipelineHash=0x") != std::string::npos &&
              report.find("drawTail[0]") != std::string::npos &&
              report.find("mapsPending=2") != std::string::npos,
          "durable report preserves frame, draw, pass, pipeline and state fields");
    check(report.find("changed since completed submit 40") != std::string::npos &&
              report.find("drawCount: 179 -> 213") != std::string::npos &&
              report.find("draw-tail delta intentionally not position-joined") !=
                  std::string::npos &&
              report.find("does not claim") != std::string::npos,
          "report compares the pending submit and states the attribution limit");
}

void test_failed_completion_is_not_a_baseline(const std::filesystem::path& directory) {
    std::puts("failed completion callback is not a completed-work baseline");
    const auto file = directory / "failed_completion.flight";
    clean_file(file);
    clean_file(file.string() + ".report.txt");
    check(configure(file, 0xFA11EDULL), "configured failed-completion fixture");
    const AuroraGpuSubmitInfo successful = submit(60);
    probe(AURORA_GPU_PROBE_SUBMIT_BEGIN, successful);
    probe(AURORA_GPU_PROBE_SUBMIT_RETURN, successful);
    complete(successful);
    const AuroraGpuSubmitInfo failed = submit(61);
    probe(AURORA_GPU_PROBE_SUBMIT_BEGIN, failed);
    probe(AURORA_GPU_PROBE_SUBMIT_RETURN, failed);
    complete(failed, AURORA_GPU_SUBMIT_STATUS_ERROR);
    const AuroraGpuSubmitInfo pending = submit(62);
    probe(AURORA_GPU_PROBE_SUBMIT_BEGIN, pending);
    device_lost("device lost after failed completion callback");
    const auto reportFile = sb::gpu_incident::report_path();
    sb::gpu_incident::shutdown();

    const auto result = sb::gpu_incident::analyze_file(file);
    check(result.ok() && result.submits.size() == 3 && result.submits[1].completion_failed() &&
              !result.submits[1].completed,
          "ERROR callback is observed but not classified as successful completion");
    std::ifstream stream(reportFile);
    const std::string report{std::istreambuf_iterator<char>{stream},
                             std::istreambuf_iterator<char>{}};
    check(report.find("changed since completed submit 60") != std::string::npos &&
              report.find("changed since completed submit 61") == std::string::npos,
          "device-loss report retains the latest successful baseline only");
}

void test_kernel_timestamp_window(const std::filesystem::path& directory) {
    std::puts("kernel timestamp causal-window control");
    const auto file = directory / "kernel_window.flight";
    clean_file(file);
    check(configure(file, 0x4B45524E454CULL), "configured kernel-window fixture");
    const AuroraGpuSubmitInfo completed = submit(50);
    probe(AURORA_GPU_PROBE_SUBMIT_BEGIN, completed);
    probe(AURORA_GPU_PROBE_SUBMIT_RETURN, completed);
    complete(completed);
    const AuroraGpuSubmitInfo pending = submit(51);
    probe(AURORA_GPU_PROBE_SUBMIT_BEGIN, pending);
    probe(AURORA_GPU_PROBE_SUBMIT_RETURN, pending);
    const AuroraGpuSubmitInfo laterCompleted = submit(52);
    probe(AURORA_GPU_PROBE_SUBMIT_BEGIN, laterCompleted);
    probe(AURORA_GPU_PROBE_SUBMIT_RETURN, laterCompleted);
    complete(laterCompleted);
    sb::gpu_incident::shutdown();
    const auto analysis = sb::gpu_incident::analyze_file(file);
    check(analysis.ok() && analysis.records.size() == 8,
          "kernel-window fixture retained earlier/later completed and callback-pending submits");
    if (!analysis.ok() || analysis.records.size() != 8)
        return;
    const std::uint64_t pendingBeginNs = analysis.records[3].realtimeNs;
    const std::uint64_t pendingReturnNs = analysis.records[4].realtimeNs;
    const auto [beginStatus, atBegin] = run_flight_dump(file, pendingBeginNs);
    check(beginStatus == 0 &&
              atBegin.find("CAUSAL-WINDOW CANDIDATE submit 51") != std::string::npos &&
              atBegin.find("queue API had not returned by the event") != std::string::npos &&
              atBegin.find("last COMPLETE before kernel event: submit 50") != std::string::npos &&
              atBegin.find("changed since completed submit 50") != std::string::npos &&
              atBegin.find("changed since completed submit 52") == std::string::npos &&
              atBegin.find("drawTail[0]") != std::string::npos,
          "BEGIN timestamp uses only earlier evidence and does not claim its later RETURN");
    const auto [returnStatus, atReturn] = run_flight_dump(file, pendingReturnNs);
    check(returnStatus == 0 &&
              atReturn.find("CAUSAL-WINDOW CANDIDATE submit 51") != std::string::npos &&
              atReturn.find("queue API had returned by the event") != std::string::npos,
          "RETURN timestamp reports that the queue API had returned by the event");
    const auto [negativeStatus, negative] = run_flight_dump(file, pendingBeginNs - 1);
    check(negativeStatus == 0 &&
              negative.find("CAUSAL-WINDOW CANDIDATE submit 51") == std::string::npos &&
              negative.find("no submit in the retained ring was outstanding") != std::string::npos,
          "pre-BEGIN timestamp is the must-not-match causal-window control");
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
    test_device_lost_report(directory);
    test_failed_completion_is_not_a_baseline(directory);
    test_kernel_timestamp_window(directory);
    test_corrupt_and_torn(directory);
    test_wrap_and_stale_rejection(directory);
    sb::gpu_incident::shutdown();

    for (const auto& entry : std::filesystem::directory_iterator(directory))
        clean_file(entry.path());
    std::filesystem::remove(directory, error);

    std::printf("%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
