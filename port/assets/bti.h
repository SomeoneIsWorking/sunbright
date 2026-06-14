// =============================================================================
// Endian-safe GameCube BTI / ResTIMG (J3D texture) header reader.
//
// A BTI file (a.k.a. ResTIMG, the JSystem texture resource) is a 0x20-byte
// BIG-ENDIAN header followed by tiled image data (and, for paletted formats, a
// palette). The decomp parses it by overlaying the `struct ResTIMG`
// (reference/sms/include/JSystem/ResTIMG.hpp) — which MISREADS width/height/
// offsets on a little-endian host. This reader is the native, endian-CORRECT
// replacement: every multi-byte field is assembled with explicit big-endian
// reads (be16/be32 from rarc.h) — never struct overlay. Portable across
// x86-64 and arm64 (no reliance on host byte order).
//
// Header layout (from ResTIMG.hpp; offsets relative to the header start):
//   0x00 u8  format            GX texture format (SbTexFormat codes)
//   0x01 u8  alphaEnabled
//   0x02 u16 width
//   0x04 u16 height
//   0x06 u8  wrapS
//   0x07 u8  wrapT
//   0x08 u8  isIndexTexture    (paletted/CI?)
//   0x09 u8  colorFormat       TLUT format (SbTlutFormat codes)
//   0x0A u16 numColors         palette entry count
//   0x0C u32 paletteOffset     palette data, relative to header start
//   0x10 u8  mipmapEnabled
//   ...
//   0x18 u8  mipmapCount
//   0x1C u32 imageDataOffset   tiled image bytes, relative to header start
//                              (0 is treated as 0x20 by the decomp)
// =============================================================================
#ifndef SMSPORT_ASSETS_BTI_H
#define SMSPORT_ASSETS_BTI_H

#include <cstddef>
#include <cstdint>

namespace smsport::assets {

// Parsed BTI header + pointers into the source buffer (NOT owned — `buf` must
// outlive the BtiTexture).
struct BtiTexture {
	uint8_t  format = 0;        // SbTexFormat (tex_decode.h)
	uint16_t width  = 0;
	uint16_t height = 0;
	uint8_t  wrapS  = 0;
	uint8_t  wrapT  = 0;
	uint8_t  isIndexTexture = 0;
	uint8_t  paletteFormat  = 0; // SbTlutFormat, valid only for CI formats
	uint16_t numColors      = 0;
	uint8_t  mipmapCount    = 0;

	const uint8_t* image   = nullptr;  // tiled image bytes (header + imageDataOffset)
	size_t         imageLen = 0;       // bytes available from `image` to end of buf
	const uint8_t* palette  = nullptr; // TLUT bytes (CI only), else nullptr
	size_t         paletteLen = 0;
};

// Parse a BTI/ResTIMG header from `buf` (length `len`). Returns true on success.
// On success `out` points into `buf`. Validates dimensions are non-zero and the
// image/palette offsets fall within the buffer.
bool bti_parse(const uint8_t* buf, size_t len, BtiTexture& out);

}  // namespace smsport::assets

#endif  // SMSPORT_ASSETS_BTI_H
