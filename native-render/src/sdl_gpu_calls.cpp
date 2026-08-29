#include <sunbright/native_render/sdl_gpu_calls.h>

namespace sb::native_render {

const SdlGpuCalls& production_sdl_gpu_calls() noexcept {
    static const SdlGpuCalls calls{
        .wasInit = &SDL_WasInit,
        .createGpuDevice = &SDL_CreateGPUDevice,
        .destroyGpuDevice = &SDL_DestroyGPUDevice,
        .claimWindow = &SDL_ClaimWindowForGPUDevice,
        .releaseWindow = &SDL_ReleaseWindowFromGPUDevice,
        .setSwapchainParameters = &SDL_SetGPUSwapchainParameters,
        .setAllowedFramesInFlight = &SDL_SetGPUAllowedFramesInFlight,
        .getWindowSizeInPixels = &SDL_GetWindowSizeInPixels,
        .getWindowFlags = &SDL_GetWindowFlags,
        .acquireSwapchainTexture = &SDL_AcquireGPUSwapchainTexture,
        .beginRenderPass = &SDL_BeginGPURenderPass,
        .endRenderPass = &SDL_EndGPURenderPass,
        .blitTexture = &SDL_BlitGPUTexture,
        .createTexture = &SDL_CreateGPUTexture,
        .releaseTexture = &SDL_ReleaseGPUTexture,
        .acquireCommandBuffer = &SDL_AcquireGPUCommandBuffer,
        .submitCommandBuffer = &SDL_SubmitGPUCommandBuffer,
        .submitAndAcquireFence = &SDL_SubmitGPUCommandBufferAndAcquireFence,
        .cancelCommandBuffer = &SDL_CancelGPUCommandBuffer,
        .queryFence = &SDL_QueryGPUFence,
        .releaseFence = &SDL_ReleaseGPUFence,
        .waitForIdle = &SDL_WaitForGPUIdle,
        .getError = &SDL_GetError,
    };
    return calls;
}

bool valid_device_calls(const SdlGpuCalls& calls) noexcept {
    return calls.wasInit != nullptr && calls.createGpuDevice != nullptr &&
           calls.destroyGpuDevice != nullptr && calls.createTexture != nullptr &&
           calls.releaseTexture != nullptr && calls.acquireCommandBuffer != nullptr &&
           calls.submitCommandBuffer != nullptr && calls.submitAndAcquireFence != nullptr &&
           calls.cancelCommandBuffer != nullptr && calls.queryFence != nullptr &&
           calls.releaseFence != nullptr && calls.waitForIdle != nullptr &&
           calls.getError != nullptr;
}

bool valid_presenter_calls(const SdlGpuCalls& calls) noexcept {
    return calls.claimWindow != nullptr && calls.releaseWindow != nullptr &&
           calls.setSwapchainParameters != nullptr && calls.setAllowedFramesInFlight != nullptr &&
           calls.getWindowSizeInPixels != nullptr && calls.getWindowFlags != nullptr &&
           calls.acquireSwapchainTexture != nullptr && calls.beginRenderPass != nullptr &&
           calls.endRenderPass != nullptr && calls.blitTexture != nullptr &&
           calls.getError != nullptr;
}

bool valid(const SdlGpuCalls& calls) noexcept {
    return valid_device_calls(calls) && valid_presenter_calls(calls);
}

} // namespace sb::native_render
