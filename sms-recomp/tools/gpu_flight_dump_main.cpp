// Post-mortem reader for sb::gpu_incident flight files. Deliberately a thin printer over
// analyze_file: there is exactly one parser of the format, and this is not a second one.
//
// Usage: gpu_flight_dump <file.flight> [--tail N] [--kernel-real-ns N] [--submit N]
//
// Exit codes: 0 analyzable, 1 present-but-unusable (stale/corrupt header), 2 usage/IO error.
// The interesting question after a device loss is WHICH submit was in flight:
//   BEGIN without RETURN        -> the process died inside the queue API (driver-side hang)
//   BEGIN+RETURN without COMPLETE -> Dawn had not delivered OnSubmittedWorkDone. This can mean GPU
//   backlog OR callback backlog; kernel timing is required to distinguish them.

#include "gpu_incident_recorder.h"
#include "gpu_incident_report.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <optional>
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
    case AURORA_GPU_PROBE_UNCAPTURED_ERROR:
        return "UNCAPTURED_ERROR";
    }
    return "?";
}

void print_record(const sb::gpu_incident::Record& record) {
    std::array<char, 40> wall{};
    const std::time_t seconds = static_cast<std::time_t>(record.realtimeNs / 1'000'000'000ULL);
    std::tm utc{};
    if (::gmtime_r(&seconds, &utc) != nullptr) {
        const std::size_t prefix =
            std::strftime(wall.data(), wall.size(), "%Y-%m-%dT%H:%M:%S", &utc);
        std::snprintf(wall.data() + prefix, wall.size() - prefix, ".%09lluZ",
                      static_cast<unsigned long long>(record.realtimeNs % 1'000'000'000ULL));
    } else {
        std::snprintf(wall.data(), wall.size(), "invalid-realtime");
    }
    std::printf("seq %-5llu %-9s wall=%s real_ns=%llu mono_ns=%llu submit=%llu "
                "frame=%u[%u] draws=%u/%u ops=%u verts=%u idx=%u passes=%u status=%u%s%s\n",
                static_cast<unsigned long long>(record.sequence), phase_name(record.phase),
                wall.data(), static_cast<unsigned long long>(record.realtimeNs),
                static_cast<unsigned long long>(record.monotonicNs),
                static_cast<unsigned long long>(record.info.submitId),
                static_cast<unsigned>(record.info.frameIndex),
                static_cast<unsigned>(record.info.frameId & 0xFFFFFFFFULL), record.info.drawCount,
                record.info.passCount, record.info.operationCount, record.info.vertexBytes,
                record.info.indexBytes, record.info.recordedPassCount, record.info.status,
                record.message.empty() ? "" : " msg=\"",
                record.message.empty() ? "" : record.message.c_str());
    if (!record.message.empty())
        std::printf("\"\n");
}

const sb::gpu_incident::Record* record_at_sequence(const sb::gpu_incident::Analysis& analysis,
                                                   std::uint64_t sequence) {
    for (const sb::gpu_incident::Record& record : analysis.records) {
        if (record.sequence == sequence)
            return &record;
    }
    return nullptr;
}

const sb::gpu_incident::Record*
latest_successful_complete_by_time(const sb::gpu_incident::Analysis& analysis,
                                   std::uint64_t notAfterRealtimeNs) {
    const sb::gpu_incident::Record* latest = nullptr;
    for (const sb::gpu_incident::Record& record : analysis.records) {
        if (record.phase == AURORA_GPU_PROBE_SUBMIT_COMPLETE &&
            record.info.status == AURORA_GPU_SUBMIT_STATUS_SUCCESS &&
            record.realtimeNs <= notAfterRealtimeNs &&
            (latest == nullptr || latest->sequence < record.sequence)) {
            latest = &record;
        }
    }
    return latest;
}

const sb::gpu_incident::Record*
latest_successful_complete_before_sequence(const sb::gpu_incident::Analysis& analysis,
                                           std::uint64_t beforeSequence) {
    const sb::gpu_incident::Record* latest = nullptr;
    for (const sb::gpu_incident::Record& record : analysis.records) {
        if (record.sequence < beforeSequence && record.phase == AURORA_GPU_PROBE_SUBMIT_COMPLETE &&
            record.info.status == AURORA_GPU_SUBMIT_STATUS_SUCCESS &&
            (latest == nullptr || latest->sequence < record.sequence)) {
            latest = &record;
        }
    }
    return latest;
}

std::optional<std::uint64_t> parse_u64(const char* text) {
    if (text == nullptr || text[0] == '\0')
        return std::nullopt;
    std::uint64_t value = 0;
    const char* const end = text + std::strlen(text);
    const auto result = std::from_chars(text, end, value, 10);
    if (result.ec != std::errc{} || result.ptr != end)
        return std::nullopt;
    return value;
}

void print_submit_diagnostic(const AuroraGpuSubmitInfo& info, const AuroraGpuSubmitInfo* reference);

void print_kernel_window(const sb::gpu_incident::Analysis& analysis,
                         std::uint64_t kernelRealtimeNs) {
    std::printf("=== KERNEL FAULT WINDOW real_ns=%llu ===\n",
                static_cast<unsigned long long>(kernelRealtimeNs));
    const sb::gpu_incident::Record* const baseline =
        latest_successful_complete_by_time(analysis, kernelRealtimeNs);
    if (baseline != nullptr) {
        std::printf("last COMPLETE before kernel event: submit %llu seq%llu real_ns=%llu\n",
                    static_cast<unsigned long long>(baseline->info.submitId),
                    static_cast<unsigned long long>(baseline->sequence),
                    static_cast<unsigned long long>(baseline->realtimeNs));
    } else {
        std::printf("last COMPLETE before kernel event: unavailable\n");
    }

    std::size_t candidates = 0;
    for (const sb::gpu_incident::SubmitState& state : analysis.submits) {
        const sb::gpu_incident::Record* const begin =
            record_at_sequence(analysis, state.beginSequence);
        if (begin == nullptr || begin->realtimeNs > kernelRealtimeNs)
            continue;
        const sb::gpu_incident::Record* const complete =
            record_at_sequence(analysis, state.completeSequence);
        if (complete != nullptr && complete->realtimeNs <= kernelRealtimeNs &&
            complete->info.status == AURORA_GPU_SUBMIT_STATUS_SUCCESS)
            continue;
        const sb::gpu_incident::Record* const returned =
            record_at_sequence(analysis, state.returnSequence);
        const bool returnedByEvent =
            returned != nullptr && returned->realtimeNs <= kernelRealtimeNs;
        ++candidates;
        std::printf(
            "CAUSAL-WINDOW CANDIDATE submit %llu: BEGIN real_ns=%llu; no COMPLETE by kernel "
            "event%s\n",
            static_cast<unsigned long long>(state.submitId),
            static_cast<unsigned long long>(begin->realtimeNs),
            returnedByEvent ? "; queue API had returned by the event"
                            : "; queue API had not returned by the event");
        if (complete != nullptr && complete->realtimeNs <= kernelRealtimeNs) {
            std::printf("completion callback before event was non-success status=%u; it is not a "
                        "completed-work watermark\n",
                        complete->info.status);
        } else if (complete != nullptr) {
            std::printf("completion callback arrived after the kernel event: seq%llu "
                        "real_ns=%llu status=%u; it cannot retroactively remove this submit from "
                        "the fault window\n",
                        static_cast<unsigned long long>(complete->sequence),
                        static_cast<unsigned long long>(complete->realtimeNs),
                        complete->info.status);
        }
        const sb::gpu_incident::Record* const candidateBaseline =
            latest_successful_complete_before_sequence(analysis, begin->sequence);
        print_submit_diagnostic(begin->info,
                                candidateBaseline == nullptr ? nullptr : &candidateBaseline->info);
    }
    if (candidates == 0) {
        std::printf("no submit in the retained ring was outstanding at the supplied event time\n");
    }
    std::printf(
        "kernel-window limit: outstanding-at-event narrows the likely command-stream window; "
        "it does not prove which draw or command faulted. Submits begun after this timestamp are "
        "aftermath and must not be labeled the origin.\n");
}

void print_submit_diagnostic(const AuroraGpuSubmitInfo& info,
                             const AuroraGpuSubmitInfo* reference) {
    std::array<char, 32 * 1024> diagnostic{};
    const std::size_t size = sb::gpu_incident::format_submit_diagnostic(
        diagnostic.data(), diagnostic.size(), info, reference);
    std::printf("%.*s", static_cast<int>(size), diagnostic.data());
}

} // namespace

int main(int argc, char** argv) {
    const char* filePath = nullptr;
    std::size_t tail = 16;
    std::optional<std::uint64_t> kernelRealtimeNs;
    std::optional<std::uint64_t> selectedSubmitId;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tail") == 0 && i + 1 < argc) {
            tail = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 0));
        } else if (std::strcmp(argv[i], "--kernel-real-ns") == 0 && i + 1 < argc) {
            kernelRealtimeNs = parse_u64(argv[++i]);
            if (!kernelRealtimeNs) {
                std::fprintf(stderr, "invalid decimal --kernel-real-ns value: %s\n", argv[i]);
                return 2;
            }
        } else if (std::strcmp(argv[i], "--submit") == 0 && i + 1 < argc) {
            selectedSubmitId = parse_u64(argv[++i]);
            if (!selectedSubmitId) {
                std::fprintf(stderr, "invalid decimal --submit value: %s\n", argv[i]);
                return 2;
            }
        } else if (argv[i][0] == '-') {
            std::fprintf(stderr, "unknown option: %s\n", argv[i]);
            return 2;
        } else {
            filePath = argv[i];
        }
    }
    if (filePath == nullptr || filePath[0] == '\0') {
        std::fprintf(stderr, "usage: gpu_flight_dump <file.flight> [--tail N] [--kernel-real-ns N] "
                             "[--submit N]\n");
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
    std::printf("records     : %zu usable, %zu corrupt/torn (%zu invalid bounds)\n",
                analysis.records.size(), analysis.corruptRecordCount,
                analysis.invalidBoundsRecordCount);
    if (analysis.records.empty()) {
        std::printf("no submits were ever recorded — the recorder saw no queue activity\n");
        return 1;
    }
    if (kernelRealtimeNs)
        print_kernel_window(analysis, *kernelRealtimeNs);
    if (selectedSubmitId) {
        const auto selected = std::ranges::find(analysis.submits, *selectedSubmitId,
                                                &sb::gpu_incident::SubmitState::submitId);
        if (selected == analysis.submits.end()) {
            std::fprintf(stderr, "submit %llu is not retained in this flight ring\n",
                         static_cast<unsigned long long>(*selectedSubmitId));
            return 1;
        }
        std::printf("=== SELECTED SUBMIT %llu ===\n",
                    static_cast<unsigned long long>(*selectedSubmitId));
        const sb::gpu_incident::Record* const begin =
            record_at_sequence(analysis, selected->beginSequence);
        const sb::gpu_incident::Record* const returned =
            record_at_sequence(analysis, selected->returnSequence);
        const sb::gpu_incident::Record* const complete =
            record_at_sequence(analysis, selected->completeSequence);
        std::printf(
            "boundaries: BEGIN=seq%llu real_ns=%llu RETURN=seq%llu real_ns=%llu "
            "CALLBACK=seq%llu real_ns=%llu",
            static_cast<unsigned long long>(selected->beginSequence),
            static_cast<unsigned long long>(begin == nullptr ? 0 : begin->realtimeNs),
            static_cast<unsigned long long>(selected->returnSequence),
            static_cast<unsigned long long>(returned == nullptr ? 0 : returned->realtimeNs),
            static_cast<unsigned long long>(selected->completeSequence),
            static_cast<unsigned long long>(complete == nullptr ? 0 : complete->realtimeNs));
        if (complete != nullptr)
            std::printf(" status=%u", complete->info.status);
        std::printf("\n");
        if (begin == nullptr) {
            std::printf("submit detail unavailable: BEGIN record fell outside the ring\n");
        } else {
            const sb::gpu_incident::Record* const baseline =
                latest_successful_complete_before_sequence(analysis, begin->sequence);
            print_submit_diagnostic(begin->info, baseline == nullptr ? nullptr : &baseline->info);
        }
        if (returned == nullptr)
            std::printf("boundary interpretation: queue API did not return in the retained ring\n");
        else if (complete == nullptr)
            std::printf("boundary interpretation: completion callback was not observed\n");
        else if (complete->info.status != AURORA_GPU_SUBMIT_STATUS_SUCCESS)
            std::printf("boundary interpretation: callback was observed but did not establish "
                        "successful completion\n");
        else
            std::printf("boundary interpretation: successful completion callback observed\n");
    }

    std::size_t apiPending = 0;
    std::size_t callbackPending = 0;
    std::size_t completed = 0;
    std::size_t completionFailed = 0;
    for (const sb::gpu_incident::SubmitState& state : analysis.submits) {
        if (state.api_pending())
            ++apiPending;
        else if (state.completion_callback_pending())
            ++callbackPending;
        else if (state.completed)
            ++completed;
        else if (state.completion_failed())
            ++completionFailed;
    }
    std::printf("submits     : %zu total, %zu SUCCESSFUL-COMPLETE, %zu FAILED/CANCELLED-CALLBACK, "
                "%zu COMPLETION-CALLBACK-PENDING "
                "(returned, OnSubmittedWorkDone not observed),"
                " %zu API-PENDING (no return)\n",
                analysis.submits.size(), completed, completionFailed, callbackPending, apiPending);
    std::printf(
        "completion semantics: only a SUCCESS callback is a completed-work watermark. Missing "
        "callbacks do not prove GPU work was unfinished, and ERROR/CANCELLED callbacks do not "
        "prove success; callback dispatch and readback/map callbacks can themselves be delayed.\n");
    const sb::gpu_incident::Record* const lastCompleted =
        latest_successful_complete_before_sequence(analysis, UINT64_MAX);
    if (lastCompleted != nullptr) {
        std::printf("newest retained COMPLETE callback: submit %llu at seq%llu\n",
                    static_cast<unsigned long long>(lastCompleted->info.submitId),
                    static_cast<unsigned long long>(lastCompleted->sequence));
    } else {
        std::printf("newest retained COMPLETE callback: unavailable\n");
    }
    std::printf("comparison policy: each pending submit uses the latest COMPLETE no later than "
                "that submit's BEGIN; a later callback is never used as its baseline.\n");
    if (!selectedSubmitId) {
        for (const sb::gpu_incident::SubmitState& state : analysis.submits) {
            if (!(state.api_pending() || state.completion_callback_pending()))
                continue;
            std::printf(
                "  IN-FLIGHT submit %llu: began@seq%llu returned@seq%llu completed@seq%llu\n",
                static_cast<unsigned long long>(state.submitId),
                static_cast<unsigned long long>(state.beginSequence),
                static_cast<unsigned long long>(state.returnSequence),
                static_cast<unsigned long long>(state.completeSequence));
            const sb::gpu_incident::Record* const begin =
                record_at_sequence(analysis, state.beginSequence);
            if (begin != nullptr) {
                const sb::gpu_incident::Record* const candidateBaseline =
                    latest_successful_complete_before_sequence(analysis, begin->sequence);
                print_submit_diagnostic(
                    begin->info, candidateBaseline == nullptr ? nullptr : &candidateBaseline->info);
            } else {
                std::printf("  submit detail unavailable: BEGIN record fell outside the ring\n");
            }
        }
    }

    for (const sb::gpu_incident::Record& record : analysis.records) {
        if (record.phase == AURORA_GPU_PROBE_DEVICE_LOST ||
            record.phase == AURORA_GPU_PROBE_UNCAPTURED_ERROR) {
            const bool uncapturedError = record.phase == AURORA_GPU_PROBE_UNCAPTURED_ERROR;
            std::printf("%s callback at seq%llu: %s\n",
                        uncapturedError ? "UNCAPTURED_ERROR" : "DEVICE_LOST",
                        static_cast<unsigned long long>(record.sequence),
                        record.message.empty() ? "no driver message" : record.message.c_str());
            if (uncapturedError) {
                std::printf("Dawn reported this uncaptured WebGPU error before Aurora's fatal "
                            "abort. The error type and bounded message are preserved above.\n");
            }
            if (record.info.submitId != 0) {
                std::printf("associated latest submission (temporal context, not causal proof):\n");
                const sb::gpu_incident::Record* associatedBegin = nullptr;
                for (const sb::gpu_incident::SubmitState& state : analysis.submits) {
                    if (state.submitId == record.info.submitId) {
                        associatedBegin = record_at_sequence(analysis, state.beginSequence);
                        break;
                    }
                }
                const sb::gpu_incident::Record* const associatedBaseline =
                    associatedBegin == nullptr ? nullptr
                                               : latest_successful_complete_before_sequence(
                                                     analysis, associatedBegin->sequence);
                print_submit_diagnostic(record.info, associatedBaseline == nullptr
                                                         ? nullptr
                                                         : &associatedBaseline->info);
            } else {
                std::printf("associated submission unavailable in this historical record\n");
            }
        }
    }
    if (!kernelRealtimeNs) {
        std::printf(
            "origin warning: DEVICE_LOST and UNCAPTURED_ERROR callbacks can arrive after the "
            "originating API or kernel event. The associated latest submit is temporal context "
            "and may be aftermath. Re-run with --kernel-real-ns from the first kernel fault to "
            "select the outstanding causal window.\n");
    }

    const std::size_t first =
        analysis.records.size() > tail ? analysis.records.size() - tail : std::size_t{0};
    std::printf("--- last %zu of %zu records ---\n", analysis.records.size() - first,
                analysis.records.size());
    for (std::size_t i = first; i < analysis.records.size(); ++i)
        print_record(analysis.records[i]);
    return 0;
}
