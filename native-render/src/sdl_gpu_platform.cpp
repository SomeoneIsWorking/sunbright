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
    error.clear();
    if (!valid(calls_)) {
        error = "SDL GPU call table is incomplete";
        return false;
    }
    if (window == nullptr) {
        error = "invalid SDL GPU platform request";
        return false;
    }
    if (ready()) {
        if (window_ == window && config_ == config)
            return true;
        error = "SDL GPU platform already owns a different window or device policy";
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
    if (!presenter_.initialize(device_, window, config.presenter, error)) {
        calls_.destroyGpuDevice(device_);
        device_ = nullptr;
        return false;
    }

    window_ = window;
    config_ = config;
    return true;
}

void SdlGpuPlatform::set_present_aspect(std::uint32_t width, std::uint32_t height) noexcept {
    presenter_.set_aspect(width, height);
}

PresentResult SdlGpuPlatform::encode_present(SDL_GPUCommandBuffer* commandBuffer,
                                             SDL_GPUTexture* source, std::uint32_t sourceWidth,
                                             std::uint32_t sourceHeight, std::string& error) {
    if (!ready()) {
        error = "SDL GPU platform is not ready to present";
        return PresentResult::Failed;
    }
    return presenter_.encode(commandBuffer, source, sourceWidth, sourceHeight, error);
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
    return device_ != nullptr && presenter_.ready();
}

SDL_GPUDevice* SdlGpuPlatform::device() const noexcept {
    return device_;
}

SDL_Window* SdlGpuPlatform::window() const noexcept {
    return window_;
}

} // namespace sb::native_render
