// =============================================================================
// Big-endian -> host byteswap for standalone J2D ResTIMG headers.
//
// J2D screen textures (the 'TIMG' resources a .blo references — J2DPicture digits,
// J2DWindow 9-slice borders, J2DTextBox glyphs) are raw big-endian GameCube data on
// disc, loaded straight out of the archive. The native J2D path (JUTTexture::storeTIMG /
// initTexObj) reads the ResTIMG header (width/height/imageDataOffset/...) directly to
// compute the texel pointer and GXTexObj — so on a little-endian host those multibyte
// fields are misread (a width of 8 reads as 0x0800 = 2048, the texel pointer goes wild).
//
// BMD-embedded TIMGs get this via bmd_swap (TEX1); standalone J2D ones did not, so the
// native renderer's textured-2D path SEGV'd decoding the first window texture. This swaps
// the ResTIMG header IN PLACE, ONCE per pointer (idempotent — storeTIMG may be called
// repeatedly on a shared resource). Only the header scalars are swapped; the palette and
// texel pixel data stay GC-native (the format decoder handles their tiling/16-bit order).
// Scope/field offsets match bmd_swap.cpp's swap_TEX1 ResTIMG case exactly.
#pragma once

namespace smsport::assets {

// Swap a BE ResTIMG header to host endianness in place (no-op if already swapped or null).
void restimg_swap_to_host(const void* timg);

}  // namespace smsport::assets
