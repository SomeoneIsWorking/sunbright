#pragma once

#include <sunbright/native_render/frame.h>

#include <SDL3/SDL_gpu.h>

#include <string>

namespace sb::native_render {

struct Semantic3dPassImpl;

struct Semantic3dPassTarget {
    SDL_GPUCommandBuffer* commandBuffer = nullptr;
    SDL_GPUTexture* colorTexture = nullptr;
    SDL_GPUTextureFormat colorFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTexture* depthTexture = nullptr;
    SDL_GPUTextureFormat depthFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
};

// Draws renderer-neutral model commands before the ordered 2D/UI pass. The accepted values are
// intentionally limited to unlit colour and decoded single-texture materials; the pass never
// accepts GX/FIFO/TEV state.
class Semantic3dPass {
  public:
    explicit Semantic3dPass(SDL_GPUDevice* device);
    ~Semantic3dPass();

    Semantic3dPass(const Semantic3dPass&) = delete;
    Semantic3dPass& operator=(const Semantic3dPass&) = delete;

    [[nodiscard]] bool initialize(std::string& error);
    [[nodiscard]] bool encode(const SemanticFrame& frame, const Semantic3dPassTarget& target,
                              std::string& error);
    [[nodiscard]] bool complete_encode(bool submitted, std::string& error) noexcept;

  private:
    Semantic3dPassImpl* impl_ = nullptr;
};

} // namespace sb::native_render
