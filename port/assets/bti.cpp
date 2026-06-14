// Endian-safe BTI / ResTIMG reader. See bti.h for the layout & rationale.
#include "bti.h"

#include "rarc.h"  // be16 / be32 — explicit big-endian scalar reads

namespace smsport::assets {

bool bti_parse(const uint8_t* buf, size_t len, BtiTexture& out) {
	out = BtiTexture{};
	if (len < 0x20)
		return false;

	out.format         = buf[0x00];
	// 0x01 alphaEnabled (unused here)
	out.width          = be16(buf + 0x02);
	out.height         = be16(buf + 0x04);
	out.wrapS          = buf[0x06];
	out.wrapT          = buf[0x07];
	out.isIndexTexture = buf[0x08];
	out.paletteFormat  = buf[0x09];
	out.numColors      = be16(buf + 0x0A);
	const uint32_t paletteOffset   = be32(buf + 0x0C);
	out.mipmapCount    = buf[0x18];
	uint32_t imageDataOffset       = be32(buf + 0x1C);

	if (out.width == 0 || out.height == 0)
		return false;

	// The decomp treats imageDataOffset==0 as the default 0x20 (JUTTexture.cpp).
	if (imageDataOffset == 0)
		imageDataOffset = 0x20;
	if (imageDataOffset >= len)
		return false;

	out.image    = buf + imageDataOffset;
	out.imageLen = len - imageDataOffset;

	// Palette: offset is relative to the header start (&mTexInfo->format), only
	// meaningful for CI/paletted formats with a non-zero offset.
	if (paletteOffset != 0 && out.numColors != 0) {
		if (paletteOffset >= len)
			return false;
		out.palette    = buf + paletteOffset;
		out.paletteLen = len - paletteOffset;
	}

	return true;
}

}  // namespace smsport::assets
