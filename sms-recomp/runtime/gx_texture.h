#pragma once
// gx_texture — decode a GameCube texture out of guest memory into RGBA8.
//
// GX textures are stored in TILES, not scanlines: 4x4 blocks for most formats (8x8 for 4-bit ones,
// and CMPR is 8x8 made of four 4x4 DXT1 sub-blocks). Reading one as if it were linear produces a
// recognisable-but-scrambled image, which is exactly the kind of "looks like a shading bug" defect
// this port is meant to avoid — so the tiling is implemented per format rather than approximated.

#include <cstdint>

#include "intrinsics.h"

// GXTexFmt values, as encoded in GXTexObj image0 bits 20..23.
enum : uint32_t {
    GX_TF_I4 = 0, GX_TF_I8 = 1, GX_TF_IA4 = 2, GX_TF_IA8 = 3,
    GX_TF_RGB565 = 4, GX_TF_RGB5A3 = 5, GX_TF_RGBA8 = 6,
    GX_TF_C4 = 8, GX_TF_C8 = 9, GX_TF_C14X2 = 10, GX_TF_CMPR = 14,
};

// Decode `format` texture data at guest address `addr` into `out` (w*h*4 RGBA8, top-left origin).
// False if the format is unsupported or the source is unreadable — the caller must treat that as a
// missing texture, never as a silently blank one.
bool gx_decode_texture(u32 addr, uint32_t w, uint32_t h, uint32_t format, uint32_t tlutAddr,
                       uint8_t* out);

// Whether this port can decode the format at all, so an unsupported one can be reported ONCE by
// name instead of drawing wrong pixels.
bool gx_texture_format_supported(uint32_t format);
const char* gx_texture_format_name(uint32_t format);
