// =============================================================================
// Big-endian -> host byteswap for J3D BMT material-table files (.bmt).
//
// A .bmt is a standalone J3D2 container ('bmt2'/'bmt3') holding a material table
// (and its textures) that overrides a model's own materials — loaded by
// J3DModelLoaderDataBase::loadMaterialTable into a J3DMaterialTable. Structurally
// it is the SAME MAT3 + TEX1 blocks a .bmd carries, minus the geometry blocks, so
// this swapper reuses bmd_swap's proven MAT3/TEX1 block swappers (bmd_blocks.h).
//
// Same "swap at load" strategy as bmd_swap (see bmd_swap.h): copy the BE buffer,
// walk the ORIGINAL bytes to learn structure, swap each multibyte field in place
// in the copy. Texel/palette pixel data stays GC-native (decoded at upload).
// Portable across x86-64 and arm64.
//
// All real SMS .bmt files observed are 'bmt3' with exactly {MAT3, TEX1} blocks.
// 'bmt2' (MAT2) is not yet covered (flagged via all_covered==false, loud) — none
// seen in SMS; add a MAT2 block swapper if one ever appears.
// =============================================================================
#ifndef SMSPORT_ASSETS_BMT_SWAP_H
#define SMSPORT_ASSETS_BMT_SWAP_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace smsport::assets {

struct BmtSwapResult {
	bool        ok             = false;  // header+block-table parsed, blocks dispatched
	bool        all_covered    = false;  // every present block had a real swapper
	uint32_t    block_num      = 0;
	uint32_t    blocks_covered = 0;      // count with a real swapper
	const char* error          = nullptr;
};

// Swap a big-endian J3D2 .bmt (`be_data`, `len`) into a host-endian copy `out`.
// `out` is resized to `len`. Check `.all_covered` before feeding `out` to the
// loader (loadMaterialTable).
BmtSwapResult bmt_swap_to_host(const uint8_t* be_data, size_t len,
                               std::vector<uint8_t>& out);

}  // namespace smsport::assets

#endif  // SMSPORT_ASSETS_BMT_SWAP_H
