#include "render_composition.h"

#include "../runtime/render/native_render.h"

#include <sunbright/native_render/sdl_gpu_platform.h>
#include <sunbright/native_render/sdl_semantic_frame_client.h>
#include <sunbright/native_render/semantic_frame_bridge.h>

#include <aurora/aurora.h>

#include <lucent/log.h>

#include <cstdlib>

namespace sb::host {
namespace {

class PipelineCompilationPause {
  public:
    PipelineCompilationPause() { aurora_pause_pipeline_compilation(); }
    ~PipelineCompilationPause() { aurora_resume_pipeline_compilation(); }
};

} // namespace

bool RenderComposition::configure(app::Renderer renderer, std::string& error) noexcept {
    error.clear();
    if (configured_) {
        error = "render composition was configured more than once";
        return false;
    }
    renderer_ = renderer;
    const native_render::SemanticFrameAuditSetting audit =
        native_render::parse_semantic_frame_audit(std::getenv("SB_SEMANTIC_FRAME_AUDIT"));
    if (audit == native_render::SemanticFrameAuditSetting::Invalid) {
        error = "SB_SEMANTIC_FRAME_AUDIT accepts only 0 or 1";
        return false;
    }
    semanticRequested_ = audit == native_render::SemanticFrameAuditSetting::Enabled;
    if (semanticRequested_ && renderer_ == app::Renderer::GxCompatibility) {
        error = "semantic frame audit cannot run with the GX compatibility renderer";
        return false;
    }
    configured_ = true;
    return true;
}

bool RenderComposition::initialize(SDL_Window* window, std::string& error) {
    error.clear();
    if (!configured_ || initialized_) {
        error = !configured_ ? "render composition was not configured"
                             : "render composition is already initialized";
        return false;
    }

    auto& platform = native_render::sdl_gpu_platform();
    if (renderer_ == app::Renderer::GxCompatibility) {
        aurora_set_presentation_enabled(false);
        sbr_render_set_present_window(window);
        const PipelineCompilationPause pause;
        if (!sbr_render_init(640, 448)) {
            error = "GX compatibility renderer could not claim presentation";
            return false;
        }
    } else if (semanticRequested_) {
        {
            const PipelineCompilationPause pause;
            if (!platform.initialize_device({}, error))
                return false;
        }
        if (!native_render::sdl_semantic_frame_client().initialize(
                platform, native_render::semantic_frame_bridge(), {}, error)) {
            std::string platformError;
            (void)platform.shutdown(platformError);
            return false;
        }
        lucent::info("semantic", "offscreen semantic 2D frame audit active at 640x480");
    }
    initialized_ = true;
    return true;
}

bool RenderComposition::encode_semantic_frame(std::string& error) {
    error.clear();
    if (!semantic_active())
        return true;
    return native_render::sdl_semantic_frame_client().encode_last_sealed(error);
}

bool RenderComposition::validate_semantic_audit(std::string& error) noexcept {
    error.clear();
    if (!semantic_active())
        return true;
    report_semantic_stats();
    return native_render::sdl_semantic_frame_client().validate_audit(error);
}

bool RenderComposition::stop_semantic_collection(std::string& error) noexcept {
    error.clear();
    auto& client = native_render::sdl_semantic_frame_client();
    return !client.ready() || client.stop_collection(error);
}

bool RenderComposition::shutdown(std::string& error) noexcept {
    error.clear();
    if (!configured_)
        return true;

    auto& client = native_render::sdl_semantic_frame_client();
    if (client.ready()) {
        report_semantic_stats();
        if (!client.shutdown(error))
            return false;
    }
    sbr_render_shutdown();
    if (!native_render::sdl_gpu_platform().shutdown(error))
        return false;
    initialized_ = false;
    return true;
}

bool RenderComposition::semantic_active() const noexcept {
    return initialized_ && native_render::sdl_semantic_frame_client().ready();
}

void RenderComposition::report_semantic_stats() noexcept {
    if (statsReported_)
        return;
    const auto& stats = native_render::sdl_semantic_frame_client().stats();
    lucent::info("semantic",
                 "offscreen summary: submitted={} completed={} nonempty={} mixed={} operations={} "
                 "pictures={} j2d-window-pictures={} glyphs={} solid-rectangles={} "
                 "j2d-fill-boxes={} j2d-window-contents={} images={} samples={} "
                 "first-nonclear-frame={} "
                 "first-nonclear-pixels={}",
                 stats.submittedFrames, stats.completedFrames, stats.nonEmptyFrames,
                 stats.mixedOperationFrames, stats.submittedOperations, stats.submittedPictures,
                 stats.submittedJ2dWindowPictures, stats.submittedGlyphs,
                 stats.submittedSolidRectangles, stats.submittedJ2dFillBoxes,
                 stats.submittedJ2dWindowContents, stats.submittedImages, stats.sampledFrames,
                 stats.firstNonClearFrame, stats.firstNonClearPixels);
    statsReported_ = true;
}

RenderComposition& render_composition() noexcept {
    static RenderComposition composition;
    return composition;
}

} // namespace sb::host
