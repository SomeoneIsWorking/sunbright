// =============================================================================
// Big-endian -> host byteswap for standalone J2D ResTLUT headers.
//
// A 'TLUT' resource is the standalone big-endian palette for an indexed (C4/C8)
// J2DPicture pane. Only the u16 numColors field at offset 0x02 needs to swap so
// JUTPalette::storeTLUT sizes the GXInitTlutObj/GXLoadTlut upload correctly
// instead of reading a byte-swapped garbage count. Mirrors timg_swap.cpp's
// idempotent-per-pointer policy.
#pragma once

namespace smsport::assets {

// Swap a BE ResTLUT header to host endianness in place (no-op if null).
void restlut_swap_to_host(const void* tlut);

}  // namespace smsport::assets
