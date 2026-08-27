#include "gpu_incident_report.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace sb::gpu_incident {
namespace {

const char* submit_kind_name(AuroraGpuSubmitKind kind) noexcept {
    switch (kind) {
    case AURORA_GPU_SUBMIT_FRAME:
        return "frame";
    case AURORA_GPU_SUBMIT_IMGUI_UPLOAD:
        return "imgui-upload";
    }
    return "unknown";
}

const char* submit_status_name(std::uint32_t status) noexcept {
    switch (status) {
    case AURORA_GPU_SUBMIT_STATUS_UNKNOWN:
        return "unknown/not-a-callback";
    case AURORA_GPU_SUBMIT_STATUS_SUCCESS:
        return "success";
    case AURORA_GPU_SUBMIT_STATUS_CALLBACK_CANCELLED:
        return "callback-cancelled";
    case AURORA_GPU_SUBMIT_STATUS_ERROR:
        return "error";
    default:
        return "unrecognized";
    }
}

void append_changed(FixedBufferWriter& writer, const char* name, std::uint64_t before,
                    std::uint64_t after, bool hexadecimal = false) noexcept {
    if (before == after)
        return;
    if (hexadecimal) {
        writer.append("    %s: 0x%016llx -> 0x%016llx\n", name,
                      static_cast<unsigned long long>(before),
                      static_cast<unsigned long long>(after));
    } else {
        writer.append("    %s: %llu -> %llu\n", name, static_cast<unsigned long long>(before),
                      static_cast<unsigned long long>(after));
    }
}

void append_pass(FixedBufferWriter& writer, std::uint32_t index,
                 const AuroraGpuPassProbe& pass) noexcept {
    const std::uint32_t msaaSamples = (pass.flags >> 8U) & 0xffU;
    writer.append("  pass[%u]: label=0x%016llx commands=%u draws=%u target=%ux%u flags=0x%08x "
                  "[observable=%u offscreen=%u resolve=%u depth=%u clearColor=%u clearDepth=%u "
                  "msaa=%u]\n",
                  index, static_cast<unsigned long long>(pass.labelHash), pass.commandCount,
                  pass.drawCount, pass.targetWidth, pass.targetHeight, pass.flags, pass.flags & 1U,
                  (pass.flags >> 1U) & 1U, (pass.flags >> 2U) & 1U, (pass.flags >> 3U) & 1U,
                  (pass.flags >> 4U) & 1U, (pass.flags >> 5U) & 1U, msaaSamples);
    writer.append("           commandHash=0x%016llx pipelineHash=0x%016llx\n",
                  static_cast<unsigned long long>(pass.commandHash),
                  static_cast<unsigned long long>(pass.pipelineHash));
}

void append_pass_changes(FixedBufferWriter& writer, std::uint32_t index,
                         const AuroraGpuPassProbe& before,
                         const AuroraGpuPassProbe& after) noexcept {
    char field[64];
#define SB_GPU_PASS_CHANGE(member, hex)                                                            \
    do {                                                                                           \
        std::snprintf(field, sizeof(field), "pass[%u].%s", index, #member);                        \
        append_changed(writer, field, before.member, after.member, hex);                           \
    } while (false)
    SB_GPU_PASS_CHANGE(labelHash, true);
    SB_GPU_PASS_CHANGE(commandHash, true);
    SB_GPU_PASS_CHANGE(pipelineHash, true);
    SB_GPU_PASS_CHANGE(commandCount, false);
    SB_GPU_PASS_CHANGE(drawCount, false);
    SB_GPU_PASS_CHANGE(targetWidth, false);
    SB_GPU_PASS_CHANGE(targetHeight, false);
    SB_GPU_PASS_CHANGE(flags, true);
#undef SB_GPU_PASS_CHANGE
}

const char* draw_kind_name(std::uint8_t kind) noexcept {
    switch (static_cast<AuroraGpuDrawKind>(kind)) {
    case AURORA_GPU_DRAW_CLEAR:
        return "clear";
    case AURORA_GPU_DRAW_GX:
        return "gx";
    case AURORA_GPU_DRAW_RML:
        return "rml";
    }
    return "unknown";
}

bool has_draw_probe_v2(const AuroraGpuSubmitInfo& info) noexcept {
    return info.version >= 2 && info.structSize >= offsetof(AuroraGpuSubmitInfo, draws);
}

std::uint32_t available_draw_probe_count(const AuroraGpuSubmitInfo& info) noexcept {
    if (!has_draw_probe_v2(info))
        return 0;
    const std::size_t bytes = info.structSize - offsetof(AuroraGpuSubmitInfo, draws);
    return static_cast<std::uint32_t>(
        std::min<std::size_t>(bytes / sizeof(AuroraGpuDrawProbe), AURORA_GPU_PROBE_MAX_DRAWS));
}

void append_draw(FixedBufferWriter& writer, std::uint32_t tailIndex,
                 const AuroraGpuDrawProbe& draw) noexcept {
    writer.append("  drawTail[%u]: ordinal=%u pass=%u command=%u kind=%s population=%u "
                  "drawHash=0x%016llx pipelineId=0x%016llx tag=0x%016llx\n",
                  tailIndex, draw.drawIndex, draw.passIndex, draw.commandIndex,
                  draw_kind_name(draw.shaderType), draw.population,
                  static_cast<unsigned long long>(draw.drawHash),
                  static_cast<unsigned long long>(draw.pipelineId),
                  static_cast<unsigned long long>(draw.tag));
    writer.append("               vertex=[%u,+%u] index=[%u,+%u] uniform=[%u,+%u] "
                  "flags=0x%02x [exact=%u ortho=%u indexed=%u deforming=%u cameraTex=%u "
                  "destAlpha=%u]\n",
                  draw.vertexOffset, draw.vertexBytes, draw.indexOffset, draw.indexBytes,
                  draw.uniformOffset, draw.uniformBytes, draw.flags,
                  (draw.flags & AURORA_GPU_DRAW_FLAG_EXACT) != 0,
                  (draw.flags & AURORA_GPU_DRAW_FLAG_ORTHOGRAPHIC) != 0,
                  (draw.flags & AURORA_GPU_DRAW_FLAG_INDEXED_POSITION) != 0,
                  (draw.flags & AURORA_GPU_DRAW_FLAG_DEFORMING) != 0,
                  (draw.flags & AURORA_GPU_DRAW_FLAG_CAMERA_TEX_MATRIX) != 0,
                  (draw.flags & AURORA_GPU_DRAW_FLAG_DEST_ALPHA) != 0);
}

} // namespace

namespace {

bool has_replay_source_fields(const AuroraGpuSubmitInfo& info) noexcept {
    constexpr std::size_t requiredSize =
        offsetof(AuroraGpuSubmitInfo, replaySourceUniformHash) + sizeof(std::uint64_t);
    return info.version >= 3 && info.structSize >= requiredSize;
}

} // namespace

bool has_replay_source_lineage(const AuroraGpuSubmitInfo& info) noexcept {
    return has_replay_source_fields(info) && info.replaySourceFrameId != 0;
}

FixedBufferWriter::FixedBufferWriter(char* destination, std::size_t capacity) noexcept
    : destination_(destination), capacity_(capacity) {
    if (capacity_ != 0)
        destination_[0] = '\0';
}

void FixedBufferWriter::append(const char* format, ...) noexcept {
    if (capacity_ == 0 || size_ >= capacity_ - 1)
        return;
    va_list arguments;
    va_start(arguments, format);
    const int count = std::vsnprintf(destination_ + size_, capacity_ - size_, format, arguments);
    va_end(arguments);
    if (count < 0)
        return;
    const std::size_t available = capacity_ - size_;
    const std::size_t requested = static_cast<std::size_t>(count);
    size_ += requested < available ? requested : available - 1;
}

std::size_t FixedBufferWriter::size() const noexcept {
    return size_;
}

std::size_t format_submit_diagnostic(char* destination, std::size_t capacity,
                                     const AuroraGpuSubmitInfo& info,
                                     const AuroraGpuSubmitInfo* completedReference,
                                     const AuroraGpuSubmitInfo* replaySourceReference) noexcept {
    FixedBufferWriter writer(destination, capacity);
    writer.append("submit=%llu kind=%s frameId=%llu frameIndex=%u replay=%u present=%u headless=%u "
                  "status=%u(%s)\n",
                  static_cast<unsigned long long>(info.submitId), submit_kind_name(info.kind),
                  static_cast<unsigned long long>(info.frameId), info.frameIndex,
                  info.replayEmission, info.presentEnabled, info.headless, info.status,
                  submit_status_name(info.status));
    writer.append(
        "  workload: passes=%u recordedPasses=%u draws=%u operations=%u textureUploads=%u "
        "textureCopies=%u\n",
        info.passCount, info.recordedPassCount, info.drawCount, info.operationCount,
        info.textureUploadCount, info.textureCopyCount);
    writer.append("  bytes: vertex=%u uniform=%u index=%u storage=%u textureUpload=%u\n",
                  info.vertexBytes, info.uniformBytes, info.indexBytes, info.storageBytes,
                  info.textureUploadBytes);
    writer.append("  caches: textures=%u tluts=%u copies=%u bindGroups=%u persistentEntries=%u "
                  "persistentBytes=%u\n",
                  info.cachedTextureObjects, info.cachedTlutObjects, info.cachedCopyTextures,
                  info.cachedBindGroups, info.persistentStorageEntries,
                  info.persistentStorageBytes);
    writer.append("  hashes: command=0x%016llx pipeline=0x%016llx\n",
                  static_cast<unsigned long long>(info.commandHash),
                  static_cast<unsigned long long>(info.pipelineHash));
    const std::uint32_t passCount =
        std::min<std::uint32_t>(info.recordedPassCount, AURORA_GPU_PROBE_MAX_PASSES);
    for (std::uint32_t index = 0; index < passCount; ++index)
        append_pass(writer, index, info.passes[index]);
    if (info.passCount > passCount) {
        writer.append("  coverage: %u pass(es) omitted by Aurora's %u-pass probe limit\n",
                      info.passCount - passCount, AURORA_GPU_PROBE_MAX_PASSES);
    }
    const bool hasDrawProbe = has_draw_probe_v2(info);
    if (hasDrawProbe) {
        writer.append("  readback: queuedThisSubmit=%u bytesThisSubmit=%u mapsPending=%u "
                      "mapsCompleted=%u mapsFailed=%u\n",
                      info.readbackQueuedThisSubmit, info.readbackBytesThisSubmit,
                      info.readbackMapsPending, info.readbackMapsCompleted,
                      info.readbackMapsFailed);
        const std::uint32_t availableDraws = available_draw_probe_count(info);
        const std::uint32_t drawCount =
            std::min<std::uint32_t>(info.recordedDrawCount, availableDraws);
        writer.append("  draw-tail: recorded=%u firstOrdinal=%u capacity=%u\n", drawCount,
                      info.firstRecordedDraw, AURORA_GPU_PROBE_MAX_DRAWS);
        for (std::uint32_t index = 0; index < drawCount; ++index)
            append_draw(writer, index, info.draws[index]);
        if (info.recordedDrawCount > drawCount) {
            writer.append("  draw-tail coverage: recorded=%u but only %u entries fit the persisted "
                          "structSize=%u\n",
                          info.recordedDrawCount, availableDraws, info.structSize);
        }
        writer.append(
            "  command coverage: draw-tail covers semantic GX/Rml/Clear gfx-pass draws only; "
            "end-frame readback copies, present blit, ImGui and profiler commands are excluded. "
            "Readback copy count/bytes and callback lifetime are reported only as aggregates.\n");
    } else {
        writer.append(
            "  draw/readback coverage: unavailable; this is a historical v%u submit record "
            "without the append-only v2 fields\n",
            info.version);
    }

    if (info.kind == AURORA_GPU_SUBMIT_FRAME) {
        const bool hasReplaySourceFields = has_replay_source_fields(info);
        const bool hasReplaySource = has_replay_source_lineage(info);
        if (hasReplaySource) {
            writer.append("  replay source lineage: frameId=%llu commandHash=0x%016llx "
                          "uniformHash=0x%016llx\n",
                          static_cast<unsigned long long>(info.replaySourceFrameId),
                          static_cast<unsigned long long>(info.replaySourceCommandHash),
                          static_cast<unsigned long long>(info.replaySourceUniformHash));
            if (info.replayEmission == 0) {
                writer.append(
                    "  replay source record: this real emission owns the source lineage\n");
            } else if (replaySourceReference != nullptr &&
                       has_replay_source_lineage(*replaySourceReference) &&
                       replaySourceReference->replayEmission == 0 &&
                       replaySourceReference->frameId == info.replaySourceFrameId) {
                writer.append("  replay source record: submit=%llu frameId=%llu (explicit real "
                              "emission BEGIN)\n",
                              static_cast<unsigned long long>(replaySourceReference->submitId),
                              static_cast<unsigned long long>(replaySourceReference->frameId));
                writer.append(
                    "    source lineage comparison: frameId=%s commandHash=%s uniformHash=%s\n",
                    replaySourceReference->replaySourceFrameId == info.replaySourceFrameId
                        ? "SAME"
                        : "CHANGED",
                    replaySourceReference->replaySourceCommandHash == info.replaySourceCommandHash
                        ? "SAME"
                        : "CHANGED",
                    replaySourceReference->replaySourceUniformHash == info.replaySourceUniformHash
                        ? "SAME"
                        : "CHANGED");
            } else {
                writer.append(
                    "  replay source record: unavailable; no retained real-emission BEGIN "
                    "matches frameId=%llu. The latest completed submit is not substituted as "
                    "source.\n",
                    static_cast<unsigned long long>(info.replaySourceFrameId));
            }
            writer.append(
                "  source lineage semantics: these hashes identify the validated "
                "pre-interpolation selected pass metadata, commands, palette conversions and "
                "resolves plus the full uniform block; ordinary commandHash may differ after "
                "interpolation. Standalone texture-copy FrameOps, attachment load/store and clear "
                "values, stencil clear and vertex/index/storage bytes are not hashed.\n");
        } else if (hasReplaySourceFields) {
            writer.append("  replay source lineage: not populated for this frame\n");
        } else {
            writer.append("  replay source lineage: unavailable; this is a historical v%u "
                          "submit record without the append-only v3 fields\n",
                          info.version);
        }
    } else {
        writer.append("  replay source lineage: not applicable to this %s submit\n",
                      submit_kind_name(info.kind));
    }

    if (completedReference != nullptr) {
        writer.append("  changed since completed submit %llu:\n",
                      static_cast<unsigned long long>(completedReference->submitId));
        const std::uint32_t comparedPasses =
            std::min({completedReference->recordedPassCount, info.recordedPassCount,
                      static_cast<std::uint32_t>(AURORA_GPU_PROBE_MAX_PASSES)});
        bool passPipelineTopologySame =
            completedReference->recordedPassCount == info.recordedPassCount;
        bool passShapeSame = completedReference->passCount == info.passCount &&
                             completedReference->recordedPassCount == info.recordedPassCount;
        for (std::uint32_t index = 0; index < comparedPasses; ++index) {
            const auto& before = completedReference->passes[index];
            const auto& after = info.passes[index];
            passPipelineTopologySame &= before.pipelineHash == after.pipelineHash;
            passShapeSame &=
                before.commandCount == after.commandCount && before.drawCount == after.drawCount &&
                before.targetWidth == after.targetWidth &&
                before.targetHeight == after.targetHeight && before.flags == after.flags;
        }
        writer.append("    topology summary: framePipeline=%s passPipelines=%s passShape=%s "
                      "commandHash=%s\n",
                      completedReference->pipelineHash == info.pipelineHash ? "SAME" : "CHANGED",
                      passPipelineTopologySame ? "SAME" : "CHANGED",
                      passShapeSame ? "SAME" : "CHANGED",
                      completedReference->commandHash == info.commandHash ? "SAME" : "CHANGED");
        if (completedReference->pipelineHash == info.pipelineHash && passPipelineTopologySame) {
            writer.append(
                "    interpretation: no unique pipeline topology is exposed versus the completed "
                "baseline; dynamic command data, resource lifetime/ranges, callback pressure, "
                "and nondeterministic driver behavior remain possible.\n");
        }
#define SB_GPU_SUBMIT_CHANGE(member, hex)                                                          \
    append_changed(writer, #member, completedReference->member, info.member, hex)
        SB_GPU_SUBMIT_CHANGE(kind, false);
        SB_GPU_SUBMIT_CHANGE(replayEmission, false);
        SB_GPU_SUBMIT_CHANGE(submitId, false);
        SB_GPU_SUBMIT_CHANGE(frameId, false);
        SB_GPU_SUBMIT_CHANGE(frameIndex, false);
        SB_GPU_SUBMIT_CHANGE(passCount, false);
        SB_GPU_SUBMIT_CHANGE(recordedPassCount, false);
        SB_GPU_SUBMIT_CHANGE(drawCount, false);
        SB_GPU_SUBMIT_CHANGE(operationCount, false);
        SB_GPU_SUBMIT_CHANGE(textureUploadCount, false);
        SB_GPU_SUBMIT_CHANGE(textureCopyCount, false);
        SB_GPU_SUBMIT_CHANGE(vertexBytes, false);
        SB_GPU_SUBMIT_CHANGE(uniformBytes, false);
        SB_GPU_SUBMIT_CHANGE(indexBytes, false);
        SB_GPU_SUBMIT_CHANGE(storageBytes, false);
        SB_GPU_SUBMIT_CHANGE(textureUploadBytes, false);
        SB_GPU_SUBMIT_CHANGE(cachedTextureObjects, false);
        SB_GPU_SUBMIT_CHANGE(cachedTlutObjects, false);
        SB_GPU_SUBMIT_CHANGE(cachedCopyTextures, false);
        SB_GPU_SUBMIT_CHANGE(cachedBindGroups, false);
        SB_GPU_SUBMIT_CHANGE(persistentStorageEntries, false);
        SB_GPU_SUBMIT_CHANGE(persistentStorageBytes, false);
        SB_GPU_SUBMIT_CHANGE(presentEnabled, false);
        SB_GPU_SUBMIT_CHANGE(headless, false);
        SB_GPU_SUBMIT_CHANGE(status, false);
        SB_GPU_SUBMIT_CHANGE(commandHash, true);
        SB_GPU_SUBMIT_CHANGE(pipelineHash, true);
        for (std::uint32_t index = 0; index < comparedPasses; ++index)
            append_pass_changes(writer, index, completedReference->passes[index],
                                info.passes[index]);
        if (hasDrawProbe && has_draw_probe_v2(*completedReference)) {
            SB_GPU_SUBMIT_CHANGE(recordedDrawCount, false);
            SB_GPU_SUBMIT_CHANGE(firstRecordedDraw, false);
            SB_GPU_SUBMIT_CHANGE(readbackQueuedThisSubmit, false);
            SB_GPU_SUBMIT_CHANGE(readbackBytesThisSubmit, false);
            SB_GPU_SUBMIT_CHANGE(readbackMapsPending, false);
            SB_GPU_SUBMIT_CHANGE(readbackMapsCompleted, false);
            SB_GPU_SUBMIT_CHANGE(readbackMapsFailed, false);
            writer.append(
                "    draw-tail delta intentionally not position-joined: tail slots and frame "
                "ordinals are not stable identities across independently recorded submits. "
                "Each tail is printed in full for evidence without manufacturing a pairing.\n");
        } else if (hasDrawProbe) {
            writer.append(
                "    draw/readback delta unavailable: completed baseline predates probe v2\n");
        }
#undef SB_GPU_SUBMIT_CHANGE
    } else {
        writer.append("  comparison: no completed submit was recorded before this submit\n");
    }
    if (hasDrawProbe) {
        writer.append(
            "  attribution limit: the semantic gfx-pass draw tail retains only the final %u draw "
            "fingerprints, "
            "ordinals and staging ranges; it does not retain decoded GPU commands or prove that "
            "one tail draw executed at the fault. Hashes cover selected recorded semantics but "
            "not the excluded operations and bytes declared above, shader/resource validity or "
            "causality; equal label hashes need not distinguish passes.\n",
            AURORA_GPU_PROBE_MAX_DRAWS);
    } else {
        writer.append(
            "  attribution limit: historical hashes show only whether recorded command/pipeline "
            "topology changed; they do not identify semantics or causality, and equal label "
            "hashes need not distinguish passes. This historical probe does not retain individual "
            "draw fingerprints, resource ranges, or decoded GPU commands.\n");
    }
    return writer.size();
}

} // namespace sb::gpu_incident
