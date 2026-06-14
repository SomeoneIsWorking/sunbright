// tex_decode — see tex_decode.h. PC-native GameCube texture decoder, no Dolphin.
//
// Faithful native reimplementation of the GameCube/Wii texture format decode
// (math cross-checked against externals/dolphin TextureDecoder_Generic.cpp +
// LookUpTables.h at implementation time; nothing linked). Output is RGBA8 in
// byte order R,G,B,A = R | G<<8 | B<<16 | A<<24.

#include "tex_decode.h"
#include <cstring>

namespace {

// ── n-bit → 8-bit channel expansion (bit replication; GameCube spec) ───────────
inline uint8_t c3to8(uint8_t v) { return (uint8_t)((v << 5) | (v << 2) | (v >> 1)); }  // 00000123→12312312
inline uint8_t c4to8(uint8_t v) { return (uint8_t)((v << 4) | v); }                    // 00001234→12341234
inline uint8_t c5to8(uint8_t v) { return (uint8_t)((v << 3) | (v >> 2)); }             // 00012345→12345123
inline uint8_t c6to8(uint8_t v) { return (uint8_t)((v << 2) | (v >> 4)); }             // 00123456→12345612

inline uint32_t make_rgba(int r, int g, int b, int a) {
    return (uint32_t)((a << 24) | (b << 16) | (g << 8) | r);
}
inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

// ── single-pixel decoders ──────────────────────────────────────────────────────
inline uint32_t px_IA8(uint16_t val) {            // val = (intensity<<8)|alpha
    int a = val & 0xFF, i = val >> 8;
    return (uint32_t)(i | (i << 8) | (i << 16) | (a << 24));
}
inline uint32_t px_RGB565(uint16_t val) {
    int r = c5to8((val >> 11) & 0x1f);
    int g = c6to8((val >> 5) & 0x3f);
    int b = c5to8(val & 0x1f);
    return (uint32_t)(r | (g << 8) | (b << 16) | (0xFF << 24));
}
inline uint32_t px_RGB5A3(uint16_t val) {
    int r, g, b, a;
    if (val & 0x8000) {                            // 1.rrrrr.ggggg.bbbbb : opaque
        r = c5to8((val >> 10) & 0x1f);
        g = c5to8((val >> 5) & 0x1f);
        b = c5to8(val & 0x1f);
        a = 0xFF;
    } else {                                       // 0.aaa.rrrr.gggg.bbbb : translucent
        a = c3to8((val >> 12) & 0x7);
        r = c4to8((val >> 8) & 0xf);
        g = c4to8((val >> 4) & 0xf);
        b = c4to8(val & 0xf);
    }
    return (uint32_t)(r | (g << 8) | (b << 16) | (a << 24));
}
// Palette lookup. TLUT entries are big-endian u16 in GC RAM; IA8 is read as-is
// (px_IA8 takes the raw byte order), RGB565/RGB5A3 are byteswapped first.
inline uint32_t px_paletted(uint16_t entry, int tlutfmt) {
    switch (tlutfmt) {
    case SB_TL_IA8:    return px_IA8(entry);
    case SB_TL_RGB565: return px_RGB565(bswap16(entry));
    case SB_TL_RGB5A3: return px_RGB5A3(bswap16(entry));
    default:           return 0;
    }
}

inline int blend38(int a, int b) { return (a * 3 + b * 5) >> 3; }  // CMPR 3/8 blend

// CMPR sub-block (DXT1-like, but GC byte order + the c1<=c2 branch differs from PC).
void decode_dxt(uint32_t* dst, const uint8_t* s, int pitch) {
    uint16_t c1 = (uint16_t)((s[0] << 8) | s[1]);   // big-endian
    uint16_t c2 = (uint16_t)((s[2] << 8) | s[3]);
    int b1 = c5to8(c1 & 0x1F),       b2 = c5to8(c2 & 0x1F);
    int g1 = c6to8((c1 >> 5) & 0x3F), g2 = c6to8((c2 >> 5) & 0x3F);
    int r1 = c5to8((c1 >> 11) & 0x1F), r2 = c5to8((c2 >> 11) & 0x1F);
    uint32_t colors[4];
    colors[0] = make_rgba(r1, g1, b1, 255);
    colors[1] = make_rgba(r2, g2, b2, 255);
    if (c1 > c2) {
        colors[2] = make_rgba(blend38(r2, r1), blend38(g2, g1), blend38(b2, b1), 255);
        colors[3] = make_rgba(blend38(r1, r2), blend38(g1, g2), blend38(b1, b2), 255);
    } else {
        colors[2] = make_rgba((r1 + r2) / 2, (g1 + g2) / 2, (b1 + b2) / 2, 255);
        colors[3] = make_rgba((r1 + r2) / 2, (g1 + g2) / 2, (b1 + b2) / 2, 0);  // transparent avg
    }
    for (int y = 0; y < 4; y++) {
        int val = s[4 + y];
        for (int x = 0; x < 4; x++) {
            dst[x] = colors[(val >> 6) & 3];
            val <<= 2;
        }
        dst += pitch;
    }
}

}  // namespace

int sb_tex_size_bytes(int width, int height, int fmt) {
    int nibbles;  // per texel
    switch (fmt) {
    case SB_TF_I4: case SB_TF_C4: case SB_TF_CMPR:        nibbles = 1; break;
    case SB_TF_I8: case SB_TF_IA4: case SB_TF_C8:         nibbles = 2; break;
    case SB_TF_IA8: case SB_TF_RGB565: case SB_TF_RGB5A3:
    case SB_TF_C14X2:                                     nibbles = 4; break;
    case SB_TF_RGBA8:                                     nibbles = 8; break;
    default:                                              nibbles = 2; break;
    }
    return (width * height * nibbles) / 2;
}

bool sb_tex_is_paletted(int fmt) {
    return fmt == SB_TF_C4 || fmt == SB_TF_C8 || fmt == SB_TF_C14X2;
}

void sb_tex_decode(uint32_t* dst, const uint8_t* src, int width, int height,
                   int fmt, const uint8_t* tlut, int tlutfmt) {
    const int W8 = (width + 7) / 8;
    const int W4 = (width + 3) / 4;

    switch (fmt) {
    case SB_TF_I4:
        for (int y = 0; y < height; y += 8)
            for (int x = 0; x < width; x += 8)
                for (int iy = 0; iy < 8; iy++, src += 4)
                    for (int ix = 0; ix < 4; ix++) {
                        int v = src[ix];
                        uint32_t i1 = c4to8(v >> 4) * 0x01010101u;
                        uint32_t i2 = c4to8(v & 0xF) * 0x01010101u;
                        dst[(y + iy) * width + x + ix * 2]     = i1;
                        dst[(y + iy) * width + x + ix * 2 + 1] = i2;
                    }
        break;

    case SB_TF_I8:
        for (int y = 0; y < height; y += 4)
            for (int x = 0; x < width; x += 8)
                for (int iy = 0; iy < 4; iy++, src += 8) {
                    uint32_t* o = dst + (y + iy) * width + x;
                    for (int ix = 0; ix < 8; ix++) {
                        uint8_t v = src[ix];
                        o[ix] = (uint32_t)(v | (v << 8) | (v << 16) | (v << 24));
                    }
                }
        break;

    case SB_TF_IA4:
        for (int y = 0; y < height; y += 4)
            for (int x = 0; x < width; x += 8)
                for (int iy = 0; iy < 4; iy++, src += 8) {
                    uint32_t* o = dst + (y + iy) * width + x;
                    for (int ix = 0; ix < 8; ix++) {
                        uint8_t v = src[ix];
                        uint8_t a = c4to8(v >> 4), l = c4to8(v & 0xF);
                        o[ix] = (uint32_t)((a << 24) | (l << 16) | (l << 8) | l);
                    }
                }
        break;

    case SB_TF_IA8:
        for (int y = 0; y < height; y += 4)
            for (int x = 0; x < width; x += 4)
                for (int iy = 0; iy < 4; iy++, src += 8) {
                    uint32_t* o = dst + (y + iy) * width + x;
                    for (int j = 0; j < 4; j++) {
                        uint16_t s = (uint16_t)(src[j*2] | (src[j*2+1] << 8));  // host order (like Dolphin's u16*)
                        o[j] = px_IA8(s);
                    }
                }
        break;

    case SB_TF_RGB565:
        for (int y = 0; y < height; y += 4)
            for (int x = 0; x < width; x += 4)
                for (int iy = 0; iy < 4; iy++, src += 8) {
                    uint32_t* o = dst + (y + iy) * width + x;
                    for (int j = 0; j < 4; j++) {
                        uint16_t s = (uint16_t)(src[j*2] | (src[j*2+1] << 8));
                        o[j] = px_RGB565(bswap16(s));   // stored big-endian
                    }
                }
        break;

    case SB_TF_RGB5A3:
        for (int y = 0; y < height; y += 4)
            for (int x = 0; x < width; x += 4)
                for (int iy = 0; iy < 4; iy++, src += 8) {
                    uint32_t* o = dst + (y + iy) * width + x;
                    for (int j = 0; j < 4; j++) {
                        uint16_t s = (uint16_t)(src[j*2] | (src[j*2+1] << 8));
                        o[j] = px_RGB5A3(bswap16(s));
                    }
                }
        break;

    case SB_TF_RGBA8:
        // Each 4x4 tile = 32 bytes AR (big-endian) then 32 bytes GB.
        for (int y = 0; y < height; y += 4)
            for (int x = 0; x < width; x += 4) {
                for (int iy = 0; iy < 4; iy++) {
                    const uint8_t* ar = src + iy * 8;        // A,R pairs
                    const uint8_t* gb = src + 32 + iy * 8;   // G,B pairs
                    uint32_t* o = dst + (y + iy) * width + x;
                    for (int j = 0; j < 4; j++) {
                        uint8_t a = ar[j*2], r = ar[j*2+1];
                        uint8_t g = gb[j*2], b = gb[j*2+1];
                        o[j] = make_rgba(r, g, b, a);
                    }
                }
                src += 64;
            }
        break;

    case SB_TF_C4:
        for (int y = 0; y < height; y += 8)
            for (int x = 0, yStep = (y / 8) * W8; x < width; x += 8, yStep++)
                for (int iy = 0, xStep = 8 * yStep; iy < 8; iy++, xStep++) {
                    const uint8_t* s = src + 4 * xStep;
                    uint32_t* o = dst + (y + iy) * width + x;
                    for (int k = 0; k < 4; k++) {
                        uint8_t v = s[k];
                        *o++ = px_paletted(((const uint16_t*)tlut)[v >> 4], tlutfmt);
                        *o++ = px_paletted(((const uint16_t*)tlut)[v & 0xF], tlutfmt);
                    }
                }
        break;

    case SB_TF_C8:
        for (int y = 0; y < height; y += 4)
            for (int x = 0, yStep = (y / 4) * W8; x < width; x += 8, yStep++)
                for (int iy = 0, xStep = 4 * yStep; iy < 4; iy++, xStep++) {
                    const uint8_t* s = src + 8 * xStep;
                    uint32_t* o = dst + (y + iy) * width + x;
                    for (int k = 0; k < 8; k++)
                        o[k] = px_paletted(((const uint16_t*)tlut)[s[k]], tlutfmt);
                }
        break;

    case SB_TF_C14X2:
        for (int y = 0; y < height; y += 4)
            for (int x = 0, yStep = (y / 4) * W4; x < width; x += 4, yStep++)
                for (int iy = 0, xStep = 4 * yStep; iy < 4; iy++, xStep++) {
                    const uint8_t* s = src + 8 * xStep;
                    uint32_t* o = dst + (y + iy) * width + x;
                    for (int k = 0; k < 4; k++) {
                        uint16_t v = bswap16((uint16_t)(s[k*2] | (s[k*2+1] << 8)));  // big-endian index
                        o[k] = px_paletted(((const uint16_t*)tlut)[v & 0x3FFF], tlutfmt);
                    }
                }
        break;

    case SB_TF_CMPR:
        for (int y = 0; y < height; y += 8)
            for (int x = 0; x < width; x += 8) {
                decode_dxt(dst + y * width + x,           src,      width); src += 8;
                decode_dxt(dst + y * width + x + 4,       src,      width); src += 8;
                decode_dxt(dst + (y + 4) * width + x,     src,      width); src += 8;
                decode_dxt(dst + (y + 4) * width + x + 4, src,      width); src += 8;
            }
        break;

    default:
        std::memset(dst, 0, (size_t)width * height * 4);
        break;
    }
}
