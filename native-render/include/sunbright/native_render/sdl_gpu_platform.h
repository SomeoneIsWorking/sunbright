#pragma once

#include <sunbright/native_render/sdl_gpu_presenter.h>

#include <SDL3/SDL_gpu.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace sb::native_render {

class SdlGpuFrameTarget;

struct SdlGpuPlatformConfig {
    bool debugMode = true;
    std::string driverName{};
    SdlGpuPresenterConfig presenter{};
    bool operator==(const SdlGpuPlatformConfig&) const = default;
};

// Process-level owner of the SDL GPU device and the one presenter that claims the application
// window. Renderer clients receive its non-owning device pointer and retain ownership only of
// their pipelines, resources, and frame targets.
class SdlGpuPlatform {
  public:
    explicit SdlGpuPlatform(const SdlGpuCalls& calls = production_sdl_gpu_calls()) noexcept;
    ~SdlGpuPlatform();

    SdlGpuPlatform(const SdlGpuPlatform&) = delete;
    SdlGpuPlatform& operator=(const SdlGpuPlatform&) = delete;

    [[nodiscard]] bool initialize(SDL_Window* window, const SdlGpuPlatformConfig& config,
                                  std::string& error);
    void set_present_aspect(std::uint32_t width, std::uint32_t height) noexcept;
    // Encodes swapchain acquisition, clear, and source blit into the borrowed command buffer. The
    // caller must submit after Presented and cancel or submit its other work after
    // WindowUnavailable. Failed means the command buffer must be canceled.
    [[nodiscard]] PresentResult encode_present(SDL_GPUCommandBuffer* commandBuffer,
                                               SDL_GPUTexture* source, std::uint32_t sourceWidth,
                                               std::uint32_t sourceHeight, std::string& error);
    // Refuses while client targets remain alive, preserving the device they still reference.
    [[nodiscard]] bool shutdown(std::string& error) noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] SDL_GPUDevice* device() const noexcept;
    [[nodiscard]] SDL_Window* window() const noexcept;

  private:
    friend class SdlGpuFrameTarget;

    SdlGpuCalls calls_{};
    SdlGpuPresenter presenter_;
    SDL_GPUDevice* device_ = nullptr;
    SDL_Window* window_ = nullptr;
    SdlGpuPlatformConfig config_{};
    std::size_t liveTargets_ = 0;
};

} // namespace sb::native_render
