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
            "offscreen summary: submitted=%llu completed=%llu nonempty=%llu draws=%llu "
            "images=%llu samples=%llu first-nonclear-frame=%llu first-nonclear-pixels=%zu",
            static_cast<unsigned long long>(stats.submittedFrames),
            static_cast<unsigned long long>(stats.completedFrames),
            static_cast<unsigned long long>(stats.nonEmptyFrames),
            static_cast<unsigned long long>(stats.submittedDraws),
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
        sb::native_render::parse_semantic_picture_audit(std::getenv("SB_SEMANTIC_PICTURE_AUDIT"));
    if (setting == sb::native_render::SemanticPictureAuditSetting::Invalid) {
        g_error = "SB_SEMANTIC_PICTURE_AUDIT accepts only 0 or 1";
        return false;
    }
    g_requested = setting == sb::native_render::SemanticPictureAuditSetting::Enabled;
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
    sb_logf("semantic", "offscreen semantic picture audit active at 640x480");
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
