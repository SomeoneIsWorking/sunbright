#pragma once

#include <sunbright/native_render/image.h>
#include <sunbright/native_render/picture.h>

#include <cstdint>
#include <vector>

class JUTTexture;

namespace sb {

struct CapturedNativeTexture {
    native_render::PictureTexture texture{};
    std::vector<std::uint8_t> rgba8{};

    [[nodiscard]] native_render::DecodedImageView image_view() const noexcept;
};

// Decodes one native-layout JUTTexture and its active palette. Every semantic J2D adapter shares
// this owner rather than maintaining its own interpretation of the class.
[[nodiscard]] bool capture_native_jut_texture(const JUTTexture& source,
                                              CapturedNativeTexture& capture, const char*& error);

} // namespace sb
