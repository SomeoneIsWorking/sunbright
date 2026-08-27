#pragma once

#include <sunbright/native_render/picture.h>

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sb::native_render {

struct PictureImage {
    std::uint64_t resource = 0;
    std::uint64_t revision = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::span<const std::uint8_t> rgba8{};
};

struct PictureFrame {
    Canvas canvas{};
    std::span<const PictureCommand> commands{};
    std::span<const PictureImage> images{};
    Color clear{};
};

struct PictureFramePixels {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba8{};
};

// A semantic J2D picture pass. The host owns the SDL device; this client owns only its shaders,
// pipeline, transient frame resources, and target. It does not know Aurora, GX, FIFO, or the recomp
// runtime and therefore cannot silently route through the compatibility renderer.
class PicturePass {
  public:
    explicit PicturePass(SDL_GPUDevice* device);
    ~PicturePass();

    PicturePass(const PicturePass&) = delete;
    PicturePass& operator=(const PicturePass&) = delete;

    [[nodiscard]] bool initialize(std::string& error);
    [[nodiscard]] bool render_and_readback(const PictureFrame& frame, PictureFramePixels& output,
                                           std::string& error);

  private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace sb::native_render
