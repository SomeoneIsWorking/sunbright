#pragma once

#include "../app/settings.h"

#include <sunbright/native_render/semantic_frame_mode.h>

#include <SDL3/SDL_video.h>

#include <string>

namespace sb::host {

class RenderComposition {
  public:
    [[nodiscard]] bool configure(app::Renderer renderer, std::string& error) noexcept;
    [[nodiscard]] bool initialize(SDL_Window* window, std::string& error);
    [[nodiscard]] bool encode_semantic_frame(std::string& error);
    [[nodiscard]] bool validate_semantic_output(std::string& error) noexcept;
    [[nodiscard]] bool stop_semantic_collection(std::string& error) noexcept;
    [[nodiscard]] bool shutdown(std::string& error) noexcept;
    [[nodiscard]] bool semantic_active() const noexcept;

  private:
    void report_semantic_stats() noexcept;

    app::Renderer renderer_ = app::Renderer::Aurora;
    bool configured_ = false;
    native_render::SemanticFrameMode semanticMode_ = native_render::SemanticFrameMode::Disabled;
    bool initialized_ = false;
    bool statsReported_ = false;
};

[[nodiscard]] RenderComposition& render_composition() noexcept;

} // namespace sb::host
