#include <sunbright/native_render/sdl_gpu_presenter.h>

#include "sdl_gpu_error.h"

#include <algorithm>

namespace sb::native_render {
PresentViewport present_viewport(std::uint32_t targetWidth, std::uint32_t targetHeight,
                                 std::uint32_t aspectWidth, std::uint32_t aspectHeight) noexcept {
    if (targetWidth == 0 || targetHeight == 0 || aspectWidth == 0 || aspectHeight == 0)
        return {};

    std::uint32_t width = targetWidth;
    std::uint32_t height = targetHeight;
    if (static_cast<std::uint64_t>(targetWidth) * aspectHeight >
        static_cast<std::uint64_t>(targetHeight) * aspectWidth) {
        width = std::max<std::uint32_t>(
            1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(targetHeight) * aspectWidth /
                                          aspectHeight));
    } else {
        height = std::max<std::uint32_t>(
            1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(targetWidth) * aspectHeight /
                                          aspectWidth));
    }
    return {(targetWidth - width) / 2, (targetHeight - height) / 2, width, height};
}

SdlGpuPresenter::SdlGpuPresenter(const SdlGpuCalls& calls) noexcept : calls_(calls) {}

SdlGpuPresenter::~SdlGpuPresenter() {
    shutdown();
}

bool SdlGpuPresenter::initialize(SDL_GPUDevice* device, SDL_Window* window,
                                 const SdlGpuPresenterConfig& config, std::string& error) {
    error.clear();
    if (!valid_presenter_calls(calls_)) {
        error = "SDL GPU presenter call table is incomplete";
        return false;
    }
    if (device == nullptr || window == nullptr || config.allowedFramesInFlight < 1 ||
        config.allowedFramesInFlight > 3) {
        error = "invalid SDL GPU presenter request";
        return false;
    }
    if (lifecycle_ == Lifecycle::Ready) {
        if (device_ == device && window_ == window && config_ == config)
            return true;
        error = "SDL GPU presenter already owns a different device, window, or policy";
        return false;
    }
    if (lifecycle_ != Lifecycle::Uninitialized) {
        error = "SDL GPU presenter has an incomplete window claim";
        return false;
    }

    if (!calls_.claimWindow(device, window)) {
        assign_sdl_error(error, "SDL GPU window claim failed", calls_);
        return false;
    }
    device_ = device;
    window_ = window;
    config_ = config;
    lifecycle_ = Lifecycle::Claimed;

    if (!calls_.setSwapchainParameters(device, window, config.composition, config.presentMode)) {
        assign_sdl_error(error, "SDL GPU swapchain policy failed", calls_);
        shutdown();
        return false;
    }
    if (!calls_.setAllowedFramesInFlight(device, config.allowedFramesInFlight)) {
        assign_sdl_error(error, "SDL GPU frames-in-flight policy failed", calls_);
        shutdown();
        return false;
    }
    lifecycle_ = Lifecycle::Ready;
    return true;
}

void SdlGpuPresenter::set_aspect(std::uint32_t width, std::uint32_t height) noexcept {
    if (width == 0 || height == 0)
        return;
    aspectWidth_ = width;
    aspectHeight_ = height;
}

PresentResult SdlGpuPresenter::encode(SDL_GPUCommandBuffer* commandBuffer, SDL_GPUTexture* source,
                                      std::uint32_t sourceWidth, std::uint32_t sourceHeight,
                                      std::string& error) {
    error.clear();
    if (lifecycle_ != Lifecycle::Ready || commandBuffer == nullptr || source == nullptr ||
        sourceWidth == 0 || sourceHeight == 0) {
        error = "invalid SDL GPU present request";
        return PresentResult::Failed;
    }

    int pixelWidth = 0;
    int pixelHeight = 0;
    if (!calls_.getWindowSizeInPixels(window_, &pixelWidth, &pixelHeight)) {
        assign_sdl_error(error, "SDL window-size query failed", calls_);
        return PresentResult::Failed;
    }
    const SDL_WindowFlags flags = calls_.getWindowFlags(window_);
    if ((flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) != 0 || pixelWidth <= 0 ||
        pixelHeight <= 0) {
        return PresentResult::WindowUnavailable;
    }

    SDL_GPUTexture* swapchain = nullptr;
    Uint32 targetWidth = 0;
    Uint32 targetHeight = 0;
    if (!calls_.acquireSwapchainTexture(commandBuffer, window_, &swapchain, &targetWidth,
                                        &targetHeight)) {
        assign_sdl_error(error, "SDL GPU swapchain acquisition failed", calls_);
        return PresentResult::Failed;
    }
    if (swapchain == nullptr || targetWidth == 0 || targetHeight == 0)
        return PresentResult::WindowUnavailable;

    SDL_GPUColorTargetInfo clearTarget{};
    clearTarget.texture = swapchain;
    clearTarget.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    clearTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    clearTarget.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* clearPass = calls_.beginRenderPass(commandBuffer, &clearTarget, 1, nullptr);
    if (clearPass == nullptr) {
        assign_sdl_error(error, "SDL GPU swapchain clear-pass creation failed", calls_);
        return PresentResult::Failed;
    }
    calls_.endRenderPass(clearPass);

    const PresentViewport viewport =
        present_viewport(targetWidth, targetHeight, aspectWidth_, aspectHeight_);
    SDL_GPUBlitInfo blit{};
    blit.source.texture = source;
    blit.source.w = sourceWidth;
    blit.source.h = sourceHeight;
    blit.destination.texture = swapchain;
    blit.destination.x = viewport.x;
    blit.destination.y = viewport.y;
    blit.destination.w = viewport.width;
    blit.destination.h = viewport.height;
    blit.load_op = SDL_GPU_LOADOP_LOAD;
    blit.filter = SDL_GPU_FILTER_LINEAR;
    calls_.blitTexture(commandBuffer, &blit);
    return PresentResult::Presented;
}

void SdlGpuPresenter::shutdown() noexcept {
    if (lifecycle_ != Lifecycle::Uninitialized && device_ != nullptr && window_ != nullptr &&
        calls_.releaseWindow != nullptr) {
        calls_.releaseWindow(device_, window_);
    }
    device_ = nullptr;
    window_ = nullptr;
    config_ = {};
    lifecycle_ = Lifecycle::Uninitialized;
}

bool SdlGpuPresenter::ready() const noexcept {
    return lifecycle_ == Lifecycle::Ready;
}

SDL_Window* SdlGpuPresenter::window() const noexcept {
    return window_;
}

} // namespace sb::native_render
