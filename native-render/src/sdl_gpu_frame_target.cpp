#include <sunbright/native_render/sdl_gpu_frame_target.h>

#include "sdl_gpu_error.h"

#include <exception>

namespace sb::native_render {
namespace {

bool valid_desc(const SdlGpuFrameTargetDesc& desc) noexcept {
    return desc.width != 0 && desc.height != 0 &&
           desc.colorFormat != SDL_GPU_TEXTUREFORMAT_INVALID &&
           (desc.colorUsage & SDL_GPU_TEXTUREUSAGE_COLOR_TARGET) != 0 &&
           (!desc.hasDepth || desc.depthFormat != SDL_GPU_TEXTUREFORMAT_INVALID);
}

} // namespace

SdlGpuFrameTarget::~SdlGpuFrameTarget() {
    shutdown();
}

bool SdlGpuFrameTarget::initialize(SdlGpuPlatform& platform, const SdlGpuFrameTargetDesc& desc,
                                   std::string& error) {
    error.clear();
    if (!valid(platform.calls_)) {
        error = "SDL GPU call table is incomplete";
        return false;
    }
    if (!platform.ready() || !valid_desc(desc)) {
        error = "invalid SDL GPU frame-target request";
        return false;
    }
    if (ready()) {
        if (device_ == platform.device() && desc_ == desc)
            return true;
        error = "SDL GPU frame target already owns a different device or descriptor";
        return false;
    }

    calls_ = platform.calls_;
    device_ = platform.device();
    desc_ = desc;

    SDL_GPUTextureCreateInfo colorInfo{};
    colorInfo.type = SDL_GPU_TEXTURETYPE_2D;
    colorInfo.format = desc.colorFormat;
    colorInfo.usage = desc.colorUsage;
    colorInfo.width = desc.width;
    colorInfo.height = desc.height;
    colorInfo.layer_count_or_depth = 1;
    colorInfo.num_levels = 1;
    colorInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
    color_ = calls_.createTexture(device_, &colorInfo);
    if (color_ == nullptr) {
        assign_sdl_error(error, "SDL GPU color-target creation failed", calls_);
        shutdown();
        return false;
    }

    if (desc.hasDepth) {
        SDL_GPUTextureCreateInfo depthInfo = colorInfo;
        depthInfo.format = desc.depthFormat;
        depthInfo.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        depth_ = calls_.createTexture(device_, &depthInfo);
        if (depth_ == nullptr) {
            assign_sdl_error(error, "SDL GPU depth-target creation failed", calls_);
            shutdown();
            return false;
        }
    }
    platform_ = &platform;
    ++platform.liveTargets_;
    return true;
}

void SdlGpuFrameTarget::shutdown() noexcept {
    if (device_ != nullptr) {
        if (depth_ != nullptr && calls_.releaseTexture != nullptr)
            calls_.releaseTexture(device_, depth_);
        if (color_ != nullptr && calls_.releaseTexture != nullptr)
            calls_.releaseTexture(device_, color_);
    }
    if (platform_ != nullptr) {
        if (platform_->liveTargets_ == 0)
            std::terminate();
        --platform_->liveTargets_;
    }
    platform_ = nullptr;
    device_ = nullptr;
    color_ = nullptr;
    depth_ = nullptr;
    desc_ = {};
    calls_ = {};
}

bool SdlGpuFrameTarget::ready() const noexcept {
    return device_ != nullptr && color_ != nullptr && (!desc_.hasDepth || depth_ != nullptr);
}

SDL_GPUDevice* SdlGpuFrameTarget::device() const noexcept {
    return device_;
}

SDL_GPUTexture* SdlGpuFrameTarget::color() const noexcept {
    return color_;
}

SDL_GPUTexture* SdlGpuFrameTarget::depth() const noexcept {
    return depth_;
}

const SdlGpuFrameTargetDesc& SdlGpuFrameTarget::desc() const noexcept {
    return desc_;
}

} // namespace sb::native_render
