// gx_texture.cpp — GameCube texture decode. See gx_texture.h for why the tiling is exact.

#include "gx_texture.h"

#include <lucent/log.h>

#include <cstring>

namespace {

inline void put(uint8_t* out, uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint8_t r, uint8_t g,
                uint8_t b, uint8_t a) {
    if (x >= w || y >= h) return;   // blocks overhang the edge when the size is not a tile multiple
    uint8_t* p = out + ((size_t)y * w + x) * 4;
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

inline void rgb565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
    g = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
    b = (uint8_t)((c & 0x1F) * 255 / 31);
}

void rgb5a3(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    if (c & 0x8000) {   // opaque: 0RRRRRGGGGGBBBBB
        r = (uint8_t)(((c >> 10) & 0x1F) * 255 / 31);
        g = (uint8_t)(((c >> 5) & 0x1F) * 255 / 31);
        b = (uint8_t)((c & 0x1F) * 255 / 31);
        a = 255;
    } else {            // translucent: 0AAARRRRGGGGBBBB
        a = (uint8_t)(((c >> 12) & 0x7) * 255 / 7);
        r = (uint8_t)(((c >> 8) & 0xF) * 255 / 15);
        g = (uint8_t)(((c >> 4) & 0xF) * 255 / 15);
        b = (uint8_t)((c & 0xF) * 255 / 15);
    }
}

} // namespace

bool gx_texture_format_supported(uint32_t f) {
    switch (f) {
    case GX_TF_I4: case GX_TF_I8: case GX_TF_IA4: case GX_TF_IA8:
    case GX_TF_RGB565: case GX_TF_RGB5A3: case GX_TF_RGBA8: case GX_TF_CMPR:
    case GX_TF_C4: case GX_TF_C8:
        return true;
    default:
        return false;
    }
}

const char* gx_texture_format_name(uint32_t f) {
    switch (f) {
    case GX_TF_I4: return "I4";        case GX_TF_I8: return "I8";
    case GX_TF_IA4: return "IA4";      case GX_TF_IA8: return "IA8";
    case GX_TF_RGB565: return "RGB565"; case GX_TF_RGB5A3: return "RGB5A3";
    case GX_TF_RGBA8: return "RGBA8";  case GX_TF_C4: return "C4";
    case GX_TF_C8: return "C8";        case GX_TF_C14X2: return "C14X2";
    case GX_TF_CMPR: return "CMPR";    default: return "?";
    }
}

bool gx_decode_texture(u32 addr, uint32_t w, uint32_t h, uint32_t format, uint32_t tlutAddr,
                       uint8_t* out) {
    if (!gx_texture_format_supported(format)) return false;
    if (sb_ram_fast(addr) == nullptr) return false;
    std::memset(out, 0, (size_t)w * h * 4);

    u32 src = addr;

    // Palette lookup for the colour-indexed formats. The TLUT holds RGB5A3 entries (SMS does not
    // use the IA8 TLUT variant); without a palette the image would be indices rendered as colour,
    // so a missing TLUT is a hard failure rather than a guess.
    auto tlut = [&](uint32_t idx, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
        const u32 e = tlutAddr + idx * 2;
        rgb5a3(sb_r16(e), r, g, b, a);
    };
    if ((format == GX_TF_C4 || format == GX_TF_C8) &&
        (tlutAddr == 0 || sb_ram_fast(tlutAddr) == nullptr))
        return false;

    switch (format) {
    case GX_TF_I4: {   // 8x8 tiles, two texels per byte
        for (uint32_t ty = 0; ty < h; ty += 8)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t y = 0; y < 8; ++y)
                    for (uint32_t x = 0; x < 8; x += 2) {
                        const uint8_t byte = sb_r8(src++);
                        const uint8_t hi = (uint8_t)((byte >> 4) * 17);   // 4-bit -> 8-bit
                        const uint8_t lo = (uint8_t)((byte & 0xF) * 17);
                        put(out, w, h, tx + x, ty + y, hi, hi, hi, hi);
                        put(out, w, h, tx + x + 1, ty + y, lo, lo, lo, lo);
                    }
        return true;
    }
    case GX_TF_I8: {   // 8x4 tiles
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 8; ++x) {
                        const uint8_t i = sb_r8(src++);
                        put(out, w, h, tx + x, ty + y, i, i, i, i);
                    }
        return true;
    }
    case GX_TF_IA4: {  // 8x4 tiles, AAAAIIII
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 8; ++x) {
                        const uint8_t byte = sb_r8(src++);
                        const uint8_t i = (uint8_t)((byte & 0xF) * 17);
                        const uint8_t a = (uint8_t)((byte >> 4) * 17);
                        put(out, w, h, tx + x, ty + y, i, i, i, a);
                    }
        return true;
    }
    case GX_TF_IA8: {  // 4x4 tiles, AA II
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 4)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x) {
                        const uint16_t v = sb_r16(src);
                        src += 2;
                        const uint8_t a = (uint8_t)(v >> 8), i = (uint8_t)(v & 0xFF);
                        put(out, w, h, tx + x, ty + y, i, i, i, a);
                    }
        return true;
    }
    case GX_TF_RGB565: {
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 4)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x) {
                        uint8_t r, g, b;
                        rgb565(sb_r16(src), r, g, b);
                        src += 2;
                        put(out, w, h, tx + x, ty + y, r, g, b, 255);
                    }
        return true;
    }
    case GX_TF_RGB5A3: {
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 4)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x) {
                        uint8_t r, g, b, a;
                        rgb5a3(sb_r16(src), r, g, b, a);
                        src += 2;
                        put(out, w, h, tx + x, ty + y, r, g, b, a);
                    }
        return true;
    }
    case GX_TF_RGBA8: {
        // 4x4 tiles stored as TWO halves: 32 bytes of AR pairs, then 32 bytes of GB pairs.
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 4) {
                uint8_t ar[16], gb[16];
                for (int i = 0; i < 16; ++i) { ar[i] = 0; gb[i] = 0; }
                uint8_t a[16], r[16], g[16], b[16];
                for (int i = 0; i < 16; ++i) { a[i] = sb_r8(src); r[i] = sb_r8(src + 1); src += 2; }
                for (int i = 0; i < 16; ++i) { g[i] = sb_r8(src); b[i] = sb_r8(src + 1); src += 2; }
                (void)ar; (void)gb;
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 4; ++x) {
                        const int i = (int)(y * 4 + x);
                        put(out, w, h, tx + x, ty + y, r[i], g[i], b[i], a[i]);
                    }
            }
        return true;
    }
    case GX_TF_C4: {
        for (uint32_t ty = 0; ty < h; ty += 8)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t y = 0; y < 8; ++y)
                    for (uint32_t x = 0; x < 8; x += 2) {
                        const uint8_t byte = sb_r8(src++);
                        uint8_t r, g, b, a;
                        tlut(byte >> 4, r, g, b, a);
                        put(out, w, h, tx + x, ty + y, r, g, b, a);
                        tlut(byte & 0xF, r, g, b, a);
                        put(out, w, h, tx + x + 1, ty + y, r, g, b, a);
                    }
        return true;
    }
    case GX_TF_C8: {
        for (uint32_t ty = 0; ty < h; ty += 4)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (uint32_t y = 0; y < 4; ++y)
                    for (uint32_t x = 0; x < 8; ++x) {
                        uint8_t r, g, b, a;
                        tlut(sb_r8(src++), r, g, b, a);
                        put(out, w, h, tx + x, ty + y, r, g, b, a);
                    }
        return true;
    }
    case GX_TF_CMPR: {
        // 8x8 tile = four 4x4 DXT1 sub-blocks. GC's variant differs from PC DXT1 in bit order
        // (the index bits are big-endian per row) and in using 3-colour mode when c0 <= c1.
        for (uint32_t ty = 0; ty < h; ty += 8)
            for (uint32_t tx = 0; tx < w; tx += 8)
                for (int sub = 0; sub < 4; ++sub) {
                    const uint32_t ox = tx + (uint32_t)(sub & 1) * 4;
                    const uint32_t oy = ty + (uint32_t)(sub >> 1) * 4;
                    const uint16_t c0 = sb_r16(src);
                    const uint16_t c1 = sb_r16(src + 2);
                    const uint32_t bits = sb_r32(src + 4);
                    src += 8;
                    uint8_t cr[4], cg[4], cb[4], ca[4];
                    rgb565(c0, cr[0], cg[0], cb[0]); ca[0] = 255;
                    rgb565(c1, cr[1], cg[1], cb[1]); ca[1] = 255;
                    if (c0 > c1) {
                        for (int k = 0; k < 3; ++k) {
                            const int a = (k == 0) ? cr[0] : (k == 1 ? cg[0] : cb[0]);
                            const int b = (k == 0) ? cr[1] : (k == 1 ? cg[1] : cb[1]);
                            uint8_t& t2 = (k == 0) ? cr[2] : (k == 1 ? cg[2] : cb[2]);
                            uint8_t& t3 = (k == 0) ? cr[3] : (k == 1 ? cg[3] : cb[3]);
                            t2 = (uint8_t)((2 * a + b) / 3);
                            t3 = (uint8_t)((a + 2 * b) / 3);
                        }
                        ca[2] = ca[3] = 255;
                    } else {
                        cr[2] = (uint8_t)((cr[0] + cr[1]) / 2);
                        cg[2] = (uint8_t)((cg[0] + cg[1]) / 2);
                        cb[2] = (uint8_t)((cb[0] + cb[1]) / 2);
                        ca[2] = 255;
                        cr[3] = cg[3] = cb[3] = 0;
                        ca[3] = 0;   // transparent black
                    }
                    for (uint32_t y = 0; y < 4; ++y)
                        for (uint32_t x = 0; x < 4; ++x) {
                            const uint32_t shift = 30 - (y * 4 + x) * 2;
                            const uint32_t i = (bits >> shift) & 3;
                            put(out, w, h, ox + x, oy + y, cr[i], cg[i], cb[i], ca[i]);
                        }
                }
        return true;
    }
    default:
        return false;
    }
}
