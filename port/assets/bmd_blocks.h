// Internal: per-block BE->host swappers shared between the BMD (.bmd/.bdl model)
// and BMT (.bmt material-table) loaders. The .bmt file format is a J3D2 container
// holding the SAME MAT3 + TEX1 blocks a .bmd does (just without the geometry
// blocks), so its material/texture tables are byteswapped by the identical block
// swappers — exposed here so bmt_swap.cpp reuses them instead of duplicating the
// (substantial, table-driven) MAT3 logic.
//
// `out` = the host-endian COPY's block base (the block header tag+size are already
// swapped by the caller), `be` = the ORIGINAL big-endian block base (read for
// structure via be16/be32), `size` = block size in bytes. See bmd_swap.cpp for the
// per-table layout documentation.
#ifndef SMSPORT_ASSETS_BMD_BLOCKS_H
#define SMSPORT_ASSETS_BMD_BLOCKS_H

#include <cstdint>

namespace smsport::assets::detail {

void swap_MAT3_block(uint8_t* out, const uint8_t* be, uint32_t size);
void swap_TEX1_block(uint8_t* out, const uint8_t* be, uint32_t size);

}  // namespace smsport::assets::detail

#endif  // SMSPORT_ASSETS_BMD_BLOCKS_H
