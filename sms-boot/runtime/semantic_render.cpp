#include "semantic_render.h"

#include <sunbright/native_render/sdl_gpu_platform.h>
#include <sunbright/native_render/sdl_semantic_frame_client.h>
#include <sunbright/native_render/semantic_frame_bridge.h>

#include <aurora/aurora.h>
#include <sb_log.h>

#include <cstdlib>
#include <string>

namespace {

bool g_configured = false;
bool g_requested = false;
bool g_statsReported = false;
std::string g_error{};

class PipelineCompilationPause {
  public:
    PipelineCompilationPause() { aurora_pause_pipeline_compilation(); }
    ~PipelineCompilationPause() { aurora_resume_pipeline_compilation(); }
};

void report_stats() {
    if (g_statsReported)
        return;
    const auto& stats = sb::native_render::sdl_semantic_frame_client().stats();
    sb_logf("semantic",
            "offscreen summary: submitted=%llu completed=%llu nonempty=%llu mixed=%llu "
            "operations=%llu "
            "pictures=%llu j2d-window-pictures=%llu glyphs=%llu solid-rectangles=%llu "
            "j2d-fill-boxes=%llu j2d-window-contents=%llu images=%llu "
            "samples=%llu "
            "first-nonclear-frame=%llu first-nonclear-pixels=%zu",
            static_cast<unsigned long long>(stats.submittedFrames),
            static_cast<unsigned long long>(stats.completedFrames),
            static_cast<unsigned long long>(stats.nonEmptyFrames),
            static_cast<unsigned long long>(stats.mixedOperationFrames),
            static_cast<unsigned long long>(stats.submittedOperations),
            static_cast<unsigned long long>(stats.submittedPictures),
            static_cast<unsigned long long>(stats.submittedJ2dWindowPictures),
            static_cast<unsigned long long>(stats.submittedGlyphs),
            static_cast<unsigned long long>(stats.submittedSolidRectangles),
            static_cast<unsigned long long>(stats.submittedJ2dFillBoxes),
            static_cast<unsigned long long>(stats.submittedJ2dWindowContents),
            static_cast<unsigned long long>(stats.submittedImages),
            static_cast<unsigned long long>(stats.sampledFrames),
            static_cast<unsigned long long>(stats.firstNonClearFrame), stats.firstNonClearPixels);
    g_statsReported = true;
}

} // namespace

extern "C" bool sb_semantic_render_configure(void) {
    g_error.clear();
    if (g_configured) {
        g_error = "semantic render composition was configured more than once";
        return false;
    }
    const auto setting =
        sb::native_render::parse_semantic_frame_audit(std::getenv("SB_SEMANTIC_FRAME_AUDIT"));
    if (setting == sb::native_render::SemanticFrameAuditSetting::Invalid) {
        g_error = "SB_SEMANTIC_FRAME_AUDIT accepts only 0 or 1";
        return false;
    }
    g_requested = setting == sb::native_render::SemanticFrameAuditSetting::Enabled;
    g_configured = true;
    return true;
}

extern "C" bool sb_semantic_render_initialize(void) {
    g_error.clear();
    if (!g_configured) {
        g_error = "semantic render composition was not configured";
        return false;
    }
    if (!g_requested)
        return true;

    auto& platform = sb::native_render::sdl_gpu_platform();
    {
        const PipelineCompilationPause pause;
        if (!platform.initialize_device({}, g_error))
            return false;
    }
    if (!sb::native_render::sdl_semantic_frame_client().initialize(
            platform, sb::native_render::semantic_frame_bridge(), {}, g_error)) {
        std::string platformError;
        (void)platform.shutdown(platformError);
        return false;
    }
    sb_logf("semantic", "offscreen semantic 2D frame audit active at 640x480");
    return true;
}

extern "C" bool sb_semantic_render_consume(void) {
    g_error.clear();
    auto& client = sb::native_render::sdl_semantic_frame_client();
    if (!client.ready())
        return true;
    return client.encode_last_sealed(g_error);
}

extern "C" bool sb_semantic_render_validate(void) {
    g_error.clear();
    auto& client = sb::native_render::sdl_semantic_frame_client();
    if (client.ready())
        report_stats();
    return !client.ready() || client.validate_audit(g_error);
}

extern "C" bool sb_semantic_render_shutdown(void) {
    g_error.clear();
    auto& client = sb::native_render::sdl_semantic_frame_client();
    if (client.ready()) {
        report_stats();
        if (!client.shutdown(g_error))
            return false;
    }
    return sb::native_render::sdl_gpu_platform().shutdown(g_error);
}

extern "C" const char* sb_semantic_render_last_error(void) {
    return g_error.c_str();
}
