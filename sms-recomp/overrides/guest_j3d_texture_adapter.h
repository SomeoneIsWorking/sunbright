#pragma once

#include "guest_byte_reader.h"

#include <sunbright/native_render/res_timg_decode.h>

#include <cstdint>

namespace sb::recomp {

// Resolves one material texture number through the retail J3DTexture table and decodes its
// ResTIMG resource. J3DTexture's retail constructor stores count at +0, resources at +4, and its
// CodeWarrior vptr at +8 (readTexture, US 0x802e8348).
[[nodiscard]] bool
capture_guest_j3d_texture(const GuestByteReader& reader, std::uint32_t textureTable,
                          std::uint16_t textureNumber, native_render::DecodedTexture& texture,
                          native_render::ResTimgDecodeError& decodeError) noexcept;

} // namespace sb::recomp
