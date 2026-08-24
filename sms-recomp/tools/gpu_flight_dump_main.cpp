// Post-mortem reader for sb::gpu_incident flight files. Deliberately a thin printer over
// analyze_file: there is exactly one parser of the format, and this is not a second one.
//
// Usage: gpu_flight_dump <file.flight> [--tail N]
//
// Exit codes: 0 analyzable, 1 present-but-unusable (stale/corrupt header), 2 usage/IO error.
// The interesting question after a device loss is WHICH submit was in flight:
//   BEGIN without RETURN        -> the process died inside the queue API (driver-side hang)
//   BEGIN+RETURN without COMPLETE -> accepted by the host queue, never finished on the GPU (ring
//   hang)

#include "gpu_incident_recorder.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

const char* phase_name(AuroraGpuProbePhase phase) {
    switch (phase) {
    case AURORA_GPU_PROBE_SUBMIT_BEGIN:
        return "BEGIN";
    case AURORA_GPU_PROBE_SUBMIT_RETURN:
        return "RETURN";
    case AURORA_GPU_PROBE_SUBMIT_COMPLETE:
        return "COMPLETE";
    case AURORA_GPU_PROBE_DEVICE_LOST:
        return "DEVICE_LOST";
    }
    return "?";
}

void print_record(const sb::gpu_incident::Record& record) {
    std::printf("seq %-5llu %-9s mono=%llu.%09llums submit=%llu frame=%u[%u] draws=%u/%u ops=%u "
                "verts=%u idx=%u passes=%u%s%s\n",
                static_cast<unsigned long long>(record.sequence), phase_name(record.phase),
                record.monotonicNs / 1'000'000'000ULL,
                (record.monotonicNs % 1'000'000'000ULL) / 1'000ULL,
                static_cast<unsigned long long>(record.info.submitId),
                static_cast<unsigned>(record.info.frameIndex),
                static_cast<unsigned>(record.info.frameId & 0xFFFFFFFFULL), record.info.drawCount,
                record.info.passCount, record.info.operationCount, record.info.vertexBytes,
                record.info.indexBytes, record.info.recordedPassCount,
                record.message.empty() ? "" : " msg=\"",
                record.message.empty() ? "" : record.message.c_str());
    if (!record.message.empty())
        std::printf("\"\n");
}

} // namespace

int main(int argc, char** argv) {
    const char* filePath = nullptr;
    std::size_t tail = 16;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tail") == 0 && i + 1 < argc) {
            tail = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 0));
        } else if (argv[i][0] == '-') {
            std::fprintf(stderr, "unknown option: %s\n", argv[i]);
            return 2;
        } else {
            filePath = argv[i];
        }
    }
    if (filePath == nullptr || filePath[0] == '\0') {
        std::fprintf(stderr, "usage: gpu_flight_dump <file.flight> [--tail N]\n");
        return 2;
    }

    // No expected ids: a post-mortem reader must accept whatever survived the crash, then report
    // whose file it is so stale evidence cannot impersonate the current incident unnoticed.
    const sb::gpu_incident::Analysis analysis = sb::gpu_incident::analyze_file(filePath);
    if (analysis.status == sb::gpu_incident::AnalyzeStatus::IoError) {
        std::fprintf(stderr, "gpu_flight_dump: %s\n", analysis.error.c_str());
        return 2;
    }
    if (analysis.status != sb::gpu_incident::AnalyzeStatus::Ok) {
        std::fprintf(stderr, "gpu_flight_dump: %s\n", analysis.error.c_str());
        return 1;
    }

    std::printf("flight file : %s\n", filePath);
    std::printf("session     : label='%s' pid=%llu session=%llx\n", analysis.sessionLabel.c_str(),
                static_cast<unsigned long long>(analysis.processId),
                static_cast<unsigned long long>(analysis.sessionId));
    std::printf("records     : %zu usable, %zu corrupt/torn\n", analysis.records.size(),
                analysis.corruptRecordCount);
    if (analysis.records.empty()) {
        std::printf("no submits were ever recorded — the recorder saw no queue activity\n");
        return 1;
    }

    std::size_t apiPending = 0;
    std::size_t gpuPending = 0;
    std::size_t completed = 0;
    for (const sb::gpu_incident::SubmitState& state : analysis.submits) {
        if (state.api_pending())
            ++apiPending;
        else if (state.gpu_pending())
            ++gpuPending;
        else if (state.completed)
            ++completed;
    }
    std::printf(
        "submits     : %zu total, %zu COMPLETED, %zu GPU-PENDING (returned, never completed),"
        " %zu API-PENDING (no return)\n",
        analysis.submits.size(), completed, gpuPending, apiPending);
    for (const sb::gpu_incident::SubmitState& state : analysis.submits) {
        if (state.api_pending() || state.gpu_pending())
            std::printf(
                "  IN-FLIGHT submit %llu: began@seq%llu returned@seq%llu completed@seq%llu\n",
                static_cast<unsigned long long>(state.submitId),
                static_cast<unsigned long long>(state.beginSequence),
                static_cast<unsigned long long>(state.returnSequence),
                static_cast<unsigned long long>(state.completeSequence));
    }

    const std::size_t first =
        analysis.records.size() > tail ? analysis.records.size() - tail : std::size_t{0};
    std::printf("--- last %zu of %zu records ---\n", analysis.records.size() - first,
                analysis.records.size());
    for (std::size_t i = first; i < analysis.records.size(); ++i)
        print_record(analysis.records[i]);
    return 0;
}
