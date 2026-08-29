#pragma once

#include <sunbright/native_render/sdl_gpu_platform.h>

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <string>

namespace sb::native_render {

struct SdlGpuFrameTargetDesc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    SDL_GPUTextureFormat colorFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
    SDL_GPUTextureFormat depthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    SDL_GPUTextureUsageFlags colorUsage =
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    bool hasDepth = true;
    bool operator==(const SdlGpuFrameTargetDesc&) const = default;
};

// Reusable owner for a renderer client's color/depth target pair. Multiple renderer clients may
// hold distinct targets on the same SdlGpuPlatform device, which permits side-by-side comparison
// without duplicating the device or window claim.
class SdlGpuFrameTarget {
  public:
    SdlGpuFrameTarget() = default;
    ~SdlGpuFrameTarget();

    SdlGpuFrameTarget(const SdlGpuFrameTarget&) = delete;
    SdlGpuFrameTarget& operator=(const SdlGpuFrameTarget&) = delete;

    [[nodiscard]] bool initialize(SdlGpuPlatform& platform, const SdlGpuFrameTargetDesc& desc,
                                  std::string& error);
    void shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] SDL_GPUDevice* device() const noexcept;
    [[nodiscard]] SDL_GPUTexture* color() const noexcept;
    [[nodiscard]] SDL_GPUTexture* depth() const noexcept;
    [[nodiscard]] const SdlGpuFrameTargetDesc& desc() const noexcept;

  private:
    SdlGpuPlatform* platform_ = nullptr;
    SdlGpuCalls calls_{};
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTexture* color_ = nullptr;
    SDL_GPUTexture* depth_ = nullptr;
    SdlGpuFrameTargetDesc desc_{};
};

} // namespace sb::native_render
