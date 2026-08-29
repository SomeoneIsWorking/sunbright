#pragma once

#include <sunbright/native_render/frame.h>

#include <SDL3/SDL_gpu.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sb::native_render {

struct PicturePassImpl;

struct PictureFramePixels {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba8{};
};

// Backend-facing target for one semantic picture encoding. Every handle is borrowed. The caller
// retains command-buffer submission and target lifetime/presentation ownership.
struct PicturePassTarget {
    SDL_GPUCommandBuffer* commandBuffer = nullptr;
    SDL_GPUTexture* colorTexture = nullptr;
    SDL_GPUTextureFormat colorFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPULoadOp loadOp = SDL_GPU_LOADOP_CLEAR;
    SDL_GPUStoreOp storeOp = SDL_GPU_STOREOP_STORE;
};

// A semantic J2D picture pass. The host owns the SDL device, command buffer, target, submission,
// and presentation. This client owns shaders, format-specific pipelines, and semantic resource
// caches. Destroy it only after the host has completed work previously encoded by this pass.
class PicturePass {
  public:
    explicit PicturePass(SDL_GPUDevice* device);
    ~PicturePass();

    PicturePass(const PicturePass&) = delete;
    PicturePass& operator=(const PicturePass&) = delete;

    [[nodiscard]] bool initialize(std::string& error);
    // Encodes uploads and draws into target.commandBuffer. It never submits, waits, reads back, or
    // takes ownership of either borrowed handle. After this returns, the caller must submit or
    // cancel the command buffer and report that outcome through complete_encode() before encoding
    // another frame. Failed encodes must be canceled and completed with submitted=false.
    [[nodiscard]] bool encode(const PictureFrame& frame, const PicturePassTarget& target,
                              std::string& error);
    [[nodiscard]] bool complete_encode(bool submitted, std::string& error) noexcept;

    [[nodiscard]] std::size_t resident_image_count() const noexcept;

    // GPU-test facade. Production code should provide its live target to encode().
    [[nodiscard]] bool render_and_readback(const PictureFrame& frame, PictureFramePixels& output,
                                           std::string& error);

  private:
    PicturePassImpl* impl_ = nullptr;
};

} // namespace sb::native_render
