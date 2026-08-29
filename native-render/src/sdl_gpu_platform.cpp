#include <sunbright/native_render/sdl_gpu_platform.h>

#include "sdl_gpu_error.h"

#include <exception>

namespace sb::native_render {
SdlGpuPlatform::SdlGpuPlatform(const SdlGpuCalls& calls) noexcept
    : calls_(calls), presenter_(calls_) {}

SdlGpuPlatform::~SdlGpuPlatform() {
    if (liveTargets_ != 0)
        std::terminate();
    std::string error;
    (void)shutdown(error);
}

bool SdlGpuPlatform::initialize(SDL_Window* window, const SdlGpuPlatformConfig& config,
                                std::string& error) {
    const bool createdHere = !ready();
    if (!initialize_device(config, error))
        return false;
    if (attach_presenter(window, config.presenter, error))
        return true;
    if (createdHere) {
        std::string shutdownError;
        (void)shutdown(shutdownError);
    }
    return false;
}

bool SdlGpuPlatform::initialize_device(const SdlGpuPlatformConfig& config, std::string& error) {
    error.clear();
    if (!valid_device_calls(calls_)) {
        error = "SDL GPU device call table is incomplete";
        return false;
    }
    if (ready()) {
        if (config_.debugMode == config.debugMode && config_.driverName == config.driverName)
            return true;
        error = "SDL GPU platform already owns a different device policy";
        return false;
    }

    if ((calls_.wasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0) {
        error = "SDL video must be initialized by the window owner";
        return false;
    }

    const char* driver = config.driverName.empty() ? nullptr : config.driverName.c_str();
    device_ = calls_.createGpuDevice(SDL_GPU_SHADERFORMAT_SPIRV, config.debugMode, driver);
    if (device_ == nullptr) {
        assign_sdl_error(error, "SDL GPU device creation failed", calls_);
        return false;
    }
    config_ = config;
    return true;
}

bool SdlGpuPlatform::attach_presenter(SDL_Window* window, const SdlGpuPresenterConfig& config,
                                      std::string& error) {
    error.clear();
    if (!valid_presenter_calls(calls_)) {
        error = "SDL GPU presenter call table is incomplete";
        return false;
    }
    if (!ready() || window == nullptr) {
        error = "SDL GPU presenter requires an initialized device and window";
        return false;
    }
    if (!presenter_.initialize(device_, window, config, error))
        return false;
    window_ = window;
    config_.presenter = config;
    return true;
}

void SdlGpuPlatform::set_present_aspect(std::uint32_t width, std::uint32_t height) noexcept {
    presenter_.set_aspect(width, height);
}

PresentResult SdlGpuPlatform::encode_present(SDL_GPUCommandBuffer* commandBuffer,
                                             SDL_GPUTexture* source, std::uint32_t sourceWidth,
                                             std::uint32_t sourceHeight, std::string& error) {
    if (!presenter_ready()) {
        error = "SDL GPU platform is not ready to present";
        return PresentResult::Failed;
    }
    return presenter_.encode(commandBuffer, source, sourceWidth, sourceHeight, error);
}

SDL_GPUCommandBuffer* SdlGpuPlatform::acquire_command_buffer(std::string& error) const noexcept {
    error.clear();
    if (!ready()) {
        error = "SDL GPU platform is not ready for command acquisition";
        return nullptr;
    }
    SDL_GPUCommandBuffer* commandBuffer = calls_.acquireCommandBuffer(device_);
    if (commandBuffer == nullptr)
        assign_sdl_error(error, "SDL GPU command-buffer acquisition failed", calls_);
    return commandBuffer;
}

bool SdlGpuPlatform::submit_command_buffer(SDL_GPUCommandBuffer* commandBuffer,
                                           std::string& error) const noexcept {
    error.clear();
    if (!ready() || commandBuffer == nullptr) {
        error = "invalid SDL GPU command-buffer submission";
        return false;
    }
    if (!calls_.submitCommandBuffer(commandBuffer)) {
        assign_sdl_error(error, "SDL GPU command-buffer submission failed", calls_);
        return false;
    }
    return true;
}

SDL_GPUFence* SdlGpuPlatform::submit_and_acquire_fence(SDL_GPUCommandBuffer* commandBuffer,
                                                       std::string& error) const noexcept {
    error.clear();
    if (!ready() || commandBuffer == nullptr) {
        error = "invalid SDL GPU fenced command-buffer submission";
        return nullptr;
    }
    SDL_GPUFence* fence = calls_.submitAndAcquireFence(commandBuffer);
    if (fence == nullptr)
        assign_sdl_error(error, "SDL GPU fenced command-buffer submission failed", calls_);
    return fence;
}

bool SdlGpuPlatform::cancel_command_buffer(SDL_GPUCommandBuffer* commandBuffer,
                                           std::string& error) const noexcept {
    error.clear();
    if (!ready() || commandBuffer == nullptr) {
        error = "invalid SDL GPU command-buffer cancellation";
        return false;
    }
    if (!calls_.cancelCommandBuffer(commandBuffer)) {
        assign_sdl_error(error, "SDL GPU command-buffer cancellation failed", calls_);
        return false;
    }
    return true;
}

bool SdlGpuPlatform::wait_idle(std::string& error) const noexcept {
    error.clear();
    if (!ready())
        return true;
    if (!calls_.waitForIdle(device_)) {
        assign_sdl_error(error, "SDL GPU idle wait failed", calls_);
        return false;
    }
    return true;
}

bool SdlGpuPlatform::fence_signaled(SDL_GPUFence* fence) const noexcept {
    return ready() && fence != nullptr && calls_.queryFence(device_, fence);
}

void SdlGpuPlatform::release_fence(SDL_GPUFence* fence) const noexcept {
    if (ready() && fence != nullptr)
        calls_.releaseFence(device_, fence);
}

bool SdlGpuPlatform::shutdown(std::string& error) noexcept {
    error.clear();
    if (liveTargets_ != 0) {
        error = "SDL GPU platform still owns live frame targets";
        return false;
    }
    presenter_.shutdown();
    if (device_ != nullptr && calls_.destroyGpuDevice != nullptr)
        calls_.destroyGpuDevice(device_);
    device_ = nullptr;
    window_ = nullptr;
    config_ = {};
    return true;
}

bool SdlGpuPlatform::ready() const noexcept {
    return device_ != nullptr;
}

bool SdlGpuPlatform::presenter_ready() const noexcept {
    return device_ != nullptr && presenter_.ready();
}

SDL_GPUDevice* SdlGpuPlatform::device() const noexcept {
    return device_;
}

SDL_Window* SdlGpuPlatform::window() const noexcept {
    return window_;
}

SdlGpuPlatform& sdl_gpu_platform() noexcept {
    static SdlGpuPlatform platform;
    return platform;
}

} // namespace sb::native_render
