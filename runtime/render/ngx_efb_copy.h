#pragma once
// Pure EFB-copy color encode + GC 4×4 tiling for the GXCopyTex writeback (own-the-framebuffer:
// serve the guest's EFB→texture copies from ngx's rendered scene color). These are the INVERSE of
// runtime/render/tex_decode.cpp for RGB565 / RGB5A3 — extracted here as the shipping functions so
// the round-trip is unit-tested (render_test `efb_copy_encode`). copytex_writeback (efb_readback_
// native.cpp) calls these; the test must validate the same code, not a fork.
#include <cstdint>

namespace ngx_efb {

// ARGB8888 → RGB565 (5/6/5, no alpha). decode: tex_decode.cpp px_RGB565.
inline uint16_t argb_to_rgb565(uint32_t c) {
    uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
// ARGB8888 → RGB5A3, OPAQUE form (top bit set → 1.rrrrr.ggggg.bbbbb). ngx scene color is opaque, so
// the translucent 0.aaa.rrrr.gggg.bbbb form is never emitted. decode: tex_decode.cpp px_RGB5A3.
inline uint16_t argb_to_rgb5a3(uint32_t c) {
    uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return (uint16_t)(0x8000u | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
}
inline uint16_t argb_to_tex16(uint32_t c, int fmt) {   // fmt 5 = RGB5A3, else RGB565
    return fmt == 5 ? argb_to_rgb5a3(c) : argb_to_rgb565(c);
}

// Byte offset of texel (dx,dy) within a width-`dw` RGB565/RGB5A3 image: 4×4 tiles row-major (tile
// (y,x) order), within a tile iy-major with each row 4 big-endian u16. MUST match the iteration in
// tex_decode.cpp's RGB565/RGB5A3 decode. dw must be a multiple of 4 (the 16-bit block width).
inline uint32_t tile_offset16(int dx, int dy, int dw) {
    uint32_t tile = (uint32_t)(dy >> 2) * (uint32_t)(dw / 4) + (uint32_t)(dx >> 2);
    return tile * 32u + (uint32_t)(dy & 3) * 8u + (uint32_t)(dx & 3) * 2u;
}

}  // namespace ngx_efb
