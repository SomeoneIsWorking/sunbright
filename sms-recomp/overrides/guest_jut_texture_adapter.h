#pragma once

#include <sunbright/native_render/picture.h>

#include "guest_byte_reader.h"

#include <cstdint>
#include <vector>

namespace sb::recomp {

struct CapturedGuestTexture {
    native_render::PictureTexture texture{};
    std::vector<std::uint8_t> rgba8{};
};

// Decodes one retail JUTTexture object and its current palette through the guest-memory reader.
// Picture and window adapters share this owner so guest layout and texture decoding cannot drift.
[[nodiscard]] bool capture_guest_jut_texture(const BigEndianGuestReader& reader,
                                             std::uint32_t textureAddress,
                                             CapturedGuestTexture& capture) noexcept;

} // namespace sb::recomp
