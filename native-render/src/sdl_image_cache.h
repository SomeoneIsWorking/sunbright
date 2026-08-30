#pragma once

#include <sunbright/native_render/image.h>
#include <sunbright/native_render/picture.h>

#include <SDL3/SDL.h>

#include <cstddef>
#include <span>
#include <string>

namespace sb::native_render {

// One implementation of decoded RGBA image upload, immutable revision caching, and sampler policy
// for every semantic GPU pass. A pass begins a frame, resolves only the textures it actually uses,
// encodes pending uploads, then commits or rolls back with the command buffer.
class SdlImageCache {
  public:
    explicit SdlImageCache(SDL_GPUDevice* device);
    ~SdlImageCache();

    SdlImageCache(const SdlImageCache&) = delete;
    SdlImageCache& operator=(const SdlImageCache&) = delete;

    [[nodiscard]] bool begin(std::span<const DecodedImageView> images, std::string& error);
    [[nodiscard]] bool resolve(const PictureTexture& texture, SDL_GPUTextureSamplerBinding& binding,
                               std::string& error);
    [[nodiscard]] bool encode_uploads(SDL_GPUCommandBuffer* commandBuffer, std::string& error);
    [[nodiscard]] bool complete(bool submitted, std::string& error) noexcept;
    [[nodiscard]] std::size_t resident_count() const noexcept;

  private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace sb::native_render
