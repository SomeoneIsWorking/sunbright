#pragma once

#include <sunbright/native_render/image.h>
#include <sunbright/native_render/picture.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sb::recomp {

// Reads raw big-endian bytes from guest memory. Keeping endian decoding in the production adapter
// means its unit test exercises the same parser used by the runtime instead of a test-only copy.
struct GuestByteReader {
    void* context = nullptr;
    bool (*read)(void* context, std::uint32_t address, void* destination,
                 std::size_t size) = nullptr;
};

struct CapturedPicture {
    native_render::PictureCommand command{};
    std::array<std::vector<std::uint8_t>, 4> rgba8{};
    std::array<native_render::DecodedImageView, 4> images{};
    std::uint8_t imageCount = 0;

    void refresh_image_views() noexcept;
    [[nodiscard]] std::span<const native_render::DecodedImageView> image_views() const noexcept;
};

// Capture the complete J2DPicture state needed by the renderer before the retained guest body can
// mutate it. Projection and clip-enabled state belong to the enclosing J2D context and are attached
// by the command sink, not guessed from stale pane fields here.
[[nodiscard]] bool capture_j2d_picture(const GuestByteReader& reader, std::uint32_t self,
                                       std::uint32_t parentMatrix,
                                       CapturedPicture& capture) noexcept;

} // namespace sb::recomp
