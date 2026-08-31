#include "render_composition.h"

#include "../overrides/semantic_j3d_adapter.h"
#include "../overrides/semantic_particle_adapter.h"
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
    semanticMode_ = native_render::parse_semantic_frame_mode(std::getenv("SB_SEMANTIC_FRAME_MODE"));
    if (semanticMode_ == native_render::SemanticFrameMode::Invalid) {
        error = "SB_SEMANTIC_FRAME_MODE accepts only off, audit, or preview";
        return false;
    }
    if (semanticMode_ != native_render::SemanticFrameMode::Disabled &&
        renderer_ == app::Renderer::GxCompatibility) {
        error = "semantic frame output cannot run with the GX compatibility renderer";
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
    } else if (semanticMode_ != native_render::SemanticFrameMode::Disabled) {
        const bool preview = semanticMode_ == native_render::SemanticFrameMode::Preview;
        if (preview)
            aurora_set_presentation_enabled(false);
        {
            const PipelineCompilationPause pause;
            if (!platform.initialize_device({}, error))
                return false;
        }
        native_render::SdlSemanticFrameClientConfig config{};
        config.presentationWindow = preview ? window : nullptr;
        if (!native_render::sdl_semantic_frame_client().initialize(
                platform, native_render::semantic_frame_bridge(), config, error)) {
            std::string platformError;
            (void)platform.shutdown(platformError);
            return false;
        }
        if (preview) {
            lucent::warn("semantic", "INCOMPLETE native semantic preview active at 640x480; "
                                     "rigid unlit single-texture J3D models and the ported 2D "
                                     "families are present; other materials, particles, lights, "
                                     "and effects are intentionally absent");
        } else {
            lucent::info("semantic", "offscreen semantic frame audit active at 640x480");
        }
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

bool RenderComposition::validate_semantic_output(std::string& error) noexcept {
    error.clear();
    if (!semantic_active())
        return true;
    report_semantic_stats();
    return native_render::sdl_semantic_frame_client().validate_output(error);
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
    lucent::info("semantic", "{}", semantic_j3d_stats_text());
    sb::recomp::report_semantic_particle_stats();
    lucent::info("semantic",
                 "semantic summary: submitted={} completed={} nonempty={} mixed={} operations={} "
                 "pictures={} j2d-window-pictures={} glyphs={} solid-rectangles={} "
                 "j2d-fill-boxes={} j2d-window-contents={} images={} samples={} presented={} "
                 "models={} meshes={} mesh-vertices={} window-unavailable={} "
                 "first-nonclear-frame={} "
                 "first-nonclear-pixels={}",
                 stats.submittedFrames, stats.completedFrames, stats.nonEmptyFrames,
                 stats.mixedOperationFrames, stats.submittedOperations, stats.submittedPictures,
                 stats.submittedJ2dWindowPictures, stats.submittedGlyphs,
                 stats.submittedSolidRectangles, stats.submittedJ2dFillBoxes,
                 stats.submittedJ2dWindowContents, stats.submittedImages, stats.sampledFrames,
                 stats.presentedFrames, stats.submittedModels, stats.submittedMeshes,
                 stats.submittedMeshVertices, stats.windowUnavailableFrames,
                 stats.firstNonClearFrame, stats.firstNonClearPixels);
    statsReported_ = true;
}

RenderComposition& render_composition() noexcept {
    static RenderComposition composition;
    return composition;
}

} // namespace sb::host
