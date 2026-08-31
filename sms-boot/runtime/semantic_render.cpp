#include "semantic_render.h"

#include "native_j3d_adapter.h"
#include "native_particle_adapter.h"

#include <sunbright/native_render/sdl_gpu_platform.h>
#include <sunbright/native_render/sdl_semantic_frame_client.h>
#include <sunbright/native_render/semantic_frame_bridge.h>
#include <sunbright/native_render/semantic_frame_mode.h>

#include <aurora/aurora.h>
#include <sb_log.h>

#include <cstdlib>
#include <string>

namespace {

bool g_configured = false;
sb::native_render::SemanticFrameMode g_mode = sb::native_render::SemanticFrameMode::Disabled;
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
            "semantic summary: submitted=%llu completed=%llu nonempty=%llu mixed=%llu "
            "operations=%llu "
            "pictures=%llu j2d-window-pictures=%llu glyphs=%llu solid-rectangles=%llu "
            "j2d-fill-boxes=%llu j2d-window-contents=%llu images=%llu models=%llu "
            "meshes=%llu mesh-vertices=%llu "
            "samples=%llu presented=%llu window-unavailable=%llu "
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
            static_cast<unsigned long long>(stats.submittedModels),
            static_cast<unsigned long long>(stats.submittedMeshes),
            static_cast<unsigned long long>(stats.submittedMeshVertices),
            static_cast<unsigned long long>(stats.sampledFrames),
            static_cast<unsigned long long>(stats.presentedFrames),
            static_cast<unsigned long long>(stats.windowUnavailableFrames),
            static_cast<unsigned long long>(stats.firstNonClearFrame), stats.firstNonClearPixels);
    sb_native_j3d_report_stats();
    sb_native_particle_report_stats();
    g_statsReported = true;
}

} // namespace

extern "C" bool sb_semantic_render_configure(void) {
    g_error.clear();
    if (g_configured) {
        g_error = "semantic render composition was configured more than once";
        return false;
    }
    g_mode = sb::native_render::parse_semantic_frame_mode(std::getenv("SB_SEMANTIC_FRAME_MODE"));
    if (g_mode == sb::native_render::SemanticFrameMode::Invalid) {
        g_error = "SB_SEMANTIC_FRAME_MODE accepts only off, audit, or preview";
        return false;
    }
    g_configured = true;
    return true;
}

extern "C" bool sb_semantic_render_initialize(SDL_Window* window) {
    g_error.clear();
    if (!g_configured) {
        g_error = "semantic render composition was not configured";
        return false;
    }
    if (g_mode == sb::native_render::SemanticFrameMode::Disabled)
        return true;

    const bool preview = g_mode == sb::native_render::SemanticFrameMode::Preview;
    if (preview)
        aurora_set_presentation_enabled(false);
    auto& platform = sb::native_render::sdl_gpu_platform();
    {
        const PipelineCompilationPause pause;
        if (!platform.initialize_device({}, g_error))
            return false;
    }
    sb::native_render::SdlSemanticFrameClientConfig config{};
    config.presentationWindow = preview ? window : nullptr;
    if (!sb::native_render::sdl_semantic_frame_client().initialize(
            platform, sb::native_render::semantic_frame_bridge(), config, g_error)) {
        std::string platformError;
        (void)platform.shutdown(platformError);
        return false;
    }
    if (preview) {
        sb_logf("semantic", "INCOMPLETE native semantic preview active at 640x480; rigid unlit "
                            "single-texture J3D models and the ported 2D families are present; "
                            "other materials, particles, lights, and effects are absent");
    } else {
        sb_logf("semantic", "offscreen semantic frame audit active at 640x480");
    }
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
    return !client.ready() || client.validate_output(g_error);
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
