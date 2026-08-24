#include "native_presenter.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>

namespace {

SDL_GPUDevice* g_device = nullptr;
SDL_Window* g_window = nullptr;
NativePresenterLifecycle g_lifecycle = NativePresenterLifecycle::Uninitialized;
std::uint32_t g_aspectWidth = 4;
std::uint32_t g_aspectHeight = 3;

void release_window_claim() noexcept {
    if (!sbr_native_presenter_shutdown_releases(g_lifecycle))
        return;

    SDL_GPUDevice* device = g_device;
    SDL_Window* window = g_window;
    g_device = nullptr;
    g_window = nullptr;
    g_lifecycle = NativePresenterLifecycle::Uninitialized;
    SDL_ReleaseWindowFromGPUDevice(device, window);
}

} // namespace

NativePresentViewport sbr_native_present_viewport(std::uint32_t targetWidth,
                                                  std::uint32_t targetHeight,
                                                  std::uint32_t aspectWidth,
                                                  std::uint32_t aspectHeight) noexcept {
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

NativePresenterInitializeAction
sbr_native_presenter_initialize_action(NativePresenterLifecycle lifecycle, bool validRequest,
                                       bool sameOwner) noexcept {
    if (!validRequest)
        return NativePresenterInitializeAction::Reject;
    if (lifecycle == NativePresenterLifecycle::Ready)
        return sameOwner ? NativePresenterInitializeAction::Reuse
                         : NativePresenterInitializeAction::Reject;
    if (lifecycle == NativePresenterLifecycle::Claimed)
        return NativePresenterInitializeAction::Reject;
    return NativePresenterInitializeAction::Claim;
}

bool sbr_native_presenter_shutdown_releases(NativePresenterLifecycle lifecycle) noexcept {
    return lifecycle != NativePresenterLifecycle::Uninitialized;
}

NativePresenterAvailability sbr_native_presenter_window_availability(bool sizeQuerySucceeded,
                                                                     bool hiddenOrMinimized,
                                                                     int pixelWidth,
                                                                     int pixelHeight) noexcept {
    if (!sizeQuerySucceeded)
        return NativePresenterAvailability::Failed;
    if (hiddenOrMinimized || pixelWidth <= 0 || pixelHeight <= 0)
        return NativePresenterAvailability::Unavailable;
    return NativePresenterAvailability::Ready;
}

NativePresenterAvailability
sbr_native_presenter_acquire_availability(bool acquireSucceeded, bool hasTexture,
                                          std::uint32_t textureWidth,
                                          std::uint32_t textureHeight) noexcept {
    if (!acquireSucceeded)
        return NativePresenterAvailability::Failed;
    if (!hasTexture || textureWidth == 0 || textureHeight == 0)
        return NativePresenterAvailability::Unavailable;
    return NativePresenterAvailability::Ready;
}

bool sbr_native_presenter_initialize(SDL_GPUDevice* device, SDL_Window* window) noexcept {
    const NativePresenterInitializeAction action =
        sbr_native_presenter_initialize_action(g_lifecycle, device != nullptr && window != nullptr,
                                               g_device == device && g_window == window);
    if (action == NativePresenterInitializeAction::Reuse)
        return true;
    if (action == NativePresenterInitializeAction::Reject)
        return false;

    if (!SDL_ClaimWindowForGPUDevice(device, window))
        return false;

    g_device = device;
    g_window = window;
    g_lifecycle = NativePresenterLifecycle::Claimed;
    if (!SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                       SDL_GPU_PRESENTMODE_VSYNC)) {
        release_window_claim();
        return false;
    }
    if (!SDL_SetGPUAllowedFramesInFlight(device, 2)) {
        release_window_claim();
        return false;
    }
    g_lifecycle = NativePresenterLifecycle::Ready;
    return true;
}

void sbr_native_presenter_set_aspect(std::uint32_t width, std::uint32_t height) noexcept {
    if (width == 0 || height == 0)
        return;
    g_aspectWidth = width;
    g_aspectHeight = height;
}

NativePresentResult sbr_native_presenter_encode(SDL_GPUCommandBuffer* commandBuffer,
                                                SDL_GPUTexture* source, std::uint32_t sourceWidth,
                                                std::uint32_t sourceHeight) noexcept {
    if (g_device == nullptr || g_window == nullptr || commandBuffer == nullptr || source == nullptr)
        return NativePresentResult::Failed;

    int pixelWidth = 0;
    int pixelHeight = 0;
    const bool sizeQuerySucceeded = SDL_GetWindowSizeInPixels(g_window, &pixelWidth, &pixelHeight);
    const SDL_WindowFlags flags = SDL_GetWindowFlags(g_window);
    const bool hiddenOrMinimized = (flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) != 0;
    const NativePresenterAvailability windowAvailability = sbr_native_presenter_window_availability(
        sizeQuerySucceeded, hiddenOrMinimized, pixelWidth, pixelHeight);
    if (windowAvailability == NativePresenterAvailability::Failed)
        return NativePresentResult::Failed;
    if (windowAvailability == NativePresenterAvailability::Unavailable)
        return NativePresentResult::WindowUnavailable;

    SDL_GPUTexture* swapchain = nullptr;
    Uint32 targetWidth = 0;
    Uint32 targetHeight = 0;
    const bool acquireSucceeded = SDL_AcquireGPUSwapchainTexture(
        commandBuffer, g_window, &swapchain, &targetWidth, &targetHeight);
    const NativePresenterAvailability acquireAvailability =
        sbr_native_presenter_acquire_availability(acquireSucceeded, swapchain != nullptr,
                                                  targetWidth, targetHeight);
    if (acquireAvailability == NativePresenterAvailability::Failed)
        return NativePresentResult::Failed;
    if (acquireAvailability == NativePresenterAvailability::Unavailable)
        return NativePresentResult::WindowUnavailable;

    SDL_GPUColorTargetInfo clearTarget{};
    clearTarget.texture = swapchain;
    clearTarget.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    clearTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    clearTarget.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* clearPass = SDL_BeginGPURenderPass(commandBuffer, &clearTarget, 1, nullptr);
    SDL_EndGPURenderPass(clearPass);

    const NativePresentViewport viewport =
        sbr_native_present_viewport(targetWidth, targetHeight, g_aspectWidth, g_aspectHeight);
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
    SDL_BlitGPUTexture(commandBuffer, &blit);
    return NativePresentResult::Presented;
}

void sbr_native_presenter_shutdown() noexcept {
    release_window_claim();
}
