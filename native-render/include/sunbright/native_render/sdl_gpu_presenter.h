#pragma once

#include <sunbright/native_render/sdl_gpu_calls.h>

#include <cstdint>
#include <string>

namespace sb::native_render {

struct PresentViewport {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool operator==(const PresentViewport&) const = default;
};

[[nodiscard]] PresentViewport present_viewport(std::uint32_t targetWidth,
                                               std::uint32_t targetHeight,
                                               std::uint32_t aspectWidth,
                                               std::uint32_t aspectHeight) noexcept;

struct SdlGpuPresenterConfig {
    SDL_GPUSwapchainComposition composition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    SDL_GPUPresentMode presentMode = SDL_GPU_PRESENTMODE_VSYNC;
    std::uint32_t allowedFramesInFlight = 2;
    bool operator==(const SdlGpuPresenterConfig&) const = default;
};

enum class PresentResult : std::uint8_t { Presented, WindowUnavailable, Failed };

// Sole owner of one SDL window's claim and swapchain policy. It never owns the device or source
// render target; those lifetimes are controlled by SdlGpuPlatform and its renderer clients.
class SdlGpuPresenter {
  public:
    explicit SdlGpuPresenter(const SdlGpuCalls& calls = production_sdl_gpu_calls()) noexcept;
    ~SdlGpuPresenter();

    SdlGpuPresenter(const SdlGpuPresenter&) = delete;
    SdlGpuPresenter& operator=(const SdlGpuPresenter&) = delete;

    [[nodiscard]] bool initialize(SDL_GPUDevice* device, SDL_Window* window,
                                  const SdlGpuPresenterConfig& config, std::string& error);
    void set_aspect(std::uint32_t width, std::uint32_t height) noexcept;
    [[nodiscard]] PresentResult encode(SDL_GPUCommandBuffer* commandBuffer, SDL_GPUTexture* source,
                                       std::uint32_t sourceWidth, std::uint32_t sourceHeight,
                                       std::string& error);
    void shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] SDL_Window* window() const noexcept;

  private:
    enum class Lifecycle : std::uint8_t { Uninitialized, Claimed, Ready };

    SdlGpuCalls calls_{};
    SDL_GPUDevice* device_ = nullptr;
    SDL_Window* window_ = nullptr;
    SdlGpuPresenterConfig config_{};
    std::uint32_t aspectWidth_ = 4;
    std::uint32_t aspectHeight_ = 3;
    Lifecycle lifecycle_ = Lifecycle::Uninitialized;
};

} // namespace sb::native_render
