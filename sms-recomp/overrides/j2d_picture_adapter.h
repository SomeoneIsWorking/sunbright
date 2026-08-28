#pragma once

#include <sunbright/native_render/image.h>
#include <sunbright/native_render/picture.h>
#include <sunbright/native_render/picture_context.h>

#include "guest_byte_reader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sb::recomp {

struct CapturedPicture {
    native_render::PictureCommand command{};
    std::array<std::vector<std::uint8_t>, 4> rgba8{};
    std::array<native_render::DecodedImageView, 4> images{};
    std::uint8_t imageCount = 0;

    void refresh_image_views() noexcept;
    [[nodiscard]] std::span<const native_render::DecodedImageView> image_views() const noexcept;
};

enum class J2DContextCaptureResult : std::uint8_t { Success, NonOrthographic, Invalid };

// J2DScreen::draw owns the logical ortho canvas and physical viewport. A null graph means the
// retail function's exact default 640x480 J2DOrthoGraph branch.
[[nodiscard]] J2DContextCaptureResult
capture_j2d_context(const GuestByteReader& reader, std::uint32_t screen, std::uint32_t grafContext,
                    native_render::PictureContext& context) noexcept;

// Capture the complete J2DPicture state needed by the renderer before the retained guest body can
// mutate it. Projection and clip-enabled state belong to the enclosing J2D context and are attached
// by the command sink, not guessed from stale pane fields here.
[[nodiscard]] bool capture_j2d_picture(const GuestByteReader& reader, std::uint32_t self,
                                       std::uint32_t parentMatrix,
                                       CapturedPicture& capture) noexcept;

} // namespace sb::recomp
