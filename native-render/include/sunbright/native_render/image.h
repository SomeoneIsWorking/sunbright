#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace sb::native_render {

// One decoded authored mip below a renderer-facing image's base level. Asset encoding, guest
// addresses, palettes, and runtime layouts have already been resolved to ordinary RGBA8 pixels.
struct DecodedImageMipLevel {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba8{};
};

// Renderer-facing image content. Mip levels exclude the base level, which remains in `rgba8` for
// single-level callers. GPU clients receive only ordinary RGBA8 pixels and semantic sampler policy.
struct DecodedImageView {
    std::uint64_t resource = 0;
    std::uint64_t revision = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::span<const std::uint8_t> rgba8{};
    std::span<const DecodedImageMipLevel> mipLevels{};
};

[[nodiscard]] bool valid(const DecodedImageView& image) noexcept;

} // namespace sb::native_render
