#pragma once
// tex_decode — N1 of the native-renderer plan (docs/native_port_plan.md §3): a
// PC-native GameCube texture decoder. ZERO Dolphin dependency.
//
// This is the first brick of the native PC game's *content* layer: it turns the
// GameCube's tiled, packed image formats into a flat RGBA8 buffer a native GPU
// can upload directly — replacing Dolphin's TextureCache/TextureDecoder. The
// recompiled game (via the J2D/J3D draw layer) supplies the raw texture bytes
// and format from the loaded BTI/BMD assets; we decode them ourselves.
//
// Output: one u32 per texel, byte order R,G,B,A (i.e. value = R | G<<8 | B<<16 |
// A<<24) — the layout a VK_FORMAT_R8G8B8A8_UNORM upload expects.
//
// The decode math is the GameCube/Wii texture format spec, re-derived natively
// (cross-checked at implementation time against externals/dolphin
// TextureDecoder_Generic.cpp + LookUpTables.h, which is the clean reference for
// the math — NOT linked). Validated byte-for-byte against Dolphin's decoder by
// the self-test in tex_decode_selftest.cpp (the /tex probe endpoint).
//
// Tiling: each format stores texels in blocks; width/height passed here are the
// in-memory (block-padded) dimensions, exactly as the asset stores them. Block
// widths: 4-bit & CMPR = 8 wide; 8-bit = 8 wide; 16-bit = 4 wide; RGBA8 = 4
// wide. Block heights: I4/CMPR = 8; everything else = 4 (RGBA8 is 4 high but
// stored as two 4x4 AR/GB tiles).

#include <cstdint>

// GameCube texture format codes (GX register values).
enum SbTexFormat {
    SB_TF_I4     = 0x0,
    SB_TF_I8     = 0x1,
    SB_TF_IA4    = 0x2,
    SB_TF_IA8    = 0x3,
    SB_TF_RGB565 = 0x4,
    SB_TF_RGB5A3 = 0x5,
    SB_TF_RGBA8  = 0x6,
    SB_TF_C4     = 0x8,
    SB_TF_C8     = 0x9,
    SB_TF_C14X2  = 0xA,
    SB_TF_CMPR   = 0xE,
};

// TLUT (palette) format codes for the paletted formats (C4/C8/C14X2).
enum SbTlutFormat {
    SB_TL_IA8    = 0x0,
    SB_TL_RGB565 = 0x1,
    SB_TL_RGB5A3 = 0x2,
};

// Bytes the source occupies for a (block-padded) width×height image in `fmt`.
int sb_tex_size_bytes(int width, int height, int fmt);

// Is `fmt` one of the paletted formats (needs a TLUT)?
bool sb_tex_is_paletted(int fmt);

// Decode `src` (tiled GC bytes) into `dst` (width*height RGBA8 texels, row-major,
// top-to-bottom). `tlut`/`tlutfmt` used only for paletted formats (else ignored).
void sb_tex_decode(uint32_t* dst, const uint8_t* src, int width, int height,
                   int fmt, const uint8_t* tlut, int tlutfmt);
