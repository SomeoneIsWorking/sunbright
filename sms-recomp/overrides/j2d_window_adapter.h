#pragma once

#include <sunbright/native_render/image.h>
#include <sunbright/native_render/picture.h>
#include <sunbright/native_render/solid_rectangle.h>

#include "guest_byte_reader.h"
#include "guest_jut_texture_adapter.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace sb::recomp {

struct CapturedWindowPicture {
    native_render::PictureCommand command{};
    std::uint8_t textureIndex = 0;
};

struct CapturedWindow {
    native_render::SolidRectangleCommand contents{};
    bool hasContents = false;
    std::array<CapturedGuestTexture, 5> textures{};
    std::array<CapturedWindowPicture, 9> pictures{};
    std::uint8_t pictureCount = 0;

    [[nodiscard]] native_render::DecodedImageView
    image_for(const CapturedWindowPicture& picture) const noexcept;
};

enum class WindowCaptureResult : std::uint8_t { Visible, Culled, Invalid };

// Captures J2DWindow::draw_private after any caller-owned rectangle/matrix edits and before the
// retained guest body runs. The guest object remains guest-layout end to end.
[[nodiscard]] WindowCaptureResult capture_j2d_window(const GuestByteReader& reader,
                                                     std::uint32_t self, std::uint32_t outerRect,
                                                     std::uint32_t contentsRect,
                                                     std::uint32_t parentMatrix,
                                                     CapturedWindow& capture) noexcept;

} // namespace sb::recomp
