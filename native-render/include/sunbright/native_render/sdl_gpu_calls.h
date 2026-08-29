#pragma once

#include <SDL3/SDL.h>

namespace sb::native_render {

// The SDL GPU platform layer calls SDL only through this table. Production uses the real SDL
// entry points; focused ownership tests install a fake table and exercise the exact same lifecycle
// code without creating a window or touching a GPU.
struct SdlGpuCalls {
    decltype(&SDL_WasInit) wasInit = nullptr;
    decltype(&SDL_CreateGPUDevice) createGpuDevice = nullptr;
    decltype(&SDL_DestroyGPUDevice) destroyGpuDevice = nullptr;
    decltype(&SDL_ClaimWindowForGPUDevice) claimWindow = nullptr;
    decltype(&SDL_ReleaseWindowFromGPUDevice) releaseWindow = nullptr;
    decltype(&SDL_SetGPUSwapchainParameters) setSwapchainParameters = nullptr;
    decltype(&SDL_SetGPUAllowedFramesInFlight) setAllowedFramesInFlight = nullptr;
    decltype(&SDL_GetWindowSizeInPixels) getWindowSizeInPixels = nullptr;
    decltype(&SDL_GetWindowFlags) getWindowFlags = nullptr;
    decltype(&SDL_AcquireGPUSwapchainTexture) acquireSwapchainTexture = nullptr;
    decltype(&SDL_BeginGPURenderPass) beginRenderPass = nullptr;
    decltype(&SDL_EndGPURenderPass) endRenderPass = nullptr;
    decltype(&SDL_BlitGPUTexture) blitTexture = nullptr;
    decltype(&SDL_CreateGPUTexture) createTexture = nullptr;
    decltype(&SDL_ReleaseGPUTexture) releaseTexture = nullptr;
    decltype(&SDL_AcquireGPUCommandBuffer) acquireCommandBuffer = nullptr;
    decltype(&SDL_SubmitGPUCommandBuffer) submitCommandBuffer = nullptr;
    decltype(&SDL_SubmitGPUCommandBufferAndAcquireFence) submitAndAcquireFence = nullptr;
    decltype(&SDL_CancelGPUCommandBuffer) cancelCommandBuffer = nullptr;
    decltype(&SDL_QueryGPUFence) queryFence = nullptr;
    decltype(&SDL_ReleaseGPUFence) releaseFence = nullptr;
    decltype(&SDL_WaitForGPUIdle) waitForIdle = nullptr;
    decltype(&SDL_GetError) getError = nullptr;
};

[[nodiscard]] const SdlGpuCalls& production_sdl_gpu_calls() noexcept;
[[nodiscard]] bool valid_device_calls(const SdlGpuCalls& calls) noexcept;
[[nodiscard]] bool valid_presenter_calls(const SdlGpuCalls& calls) noexcept;
[[nodiscard]] bool valid(const SdlGpuCalls& calls) noexcept;

} // namespace sb::native_render
