// =============================================================================
// Big-endian -> host byteswap for J3D BMD/BDL model files (the BE asset layer,
// Path A of docs/re_notes/ (the flip-era note it named was deleted with that era)).
//
// The port's core decomp loader (port/src/J3DModelLoader.cpp) reads BMD multibyte
// fields by casting raw file bytes to C structs. The decomp ran big-endian; the
// host is little-endian, so every u16/u32/f32 (and the 4-char fourcc tags it
// compares as u32 constants) is misread. This module produces a HOST-ENDIAN copy
// of a BMD so the pristine decomp loader reads it correctly — "swap at load",
// keeping the decomp untouched.
//
// Strategy: copy the BE buffer, then walk the ORIGINAL BE bytes (explicit be16/
// be32, never struct overlay) to learn the structure, swapping each multibyte
// field IN PLACE in the copy. Byte-stream regions (GX display lists, texel/
// palette data, string tables, draw-flag u8 arrays) are left untouched.
//
// Portable across x86-64 and arm64 (no host-byte-order assumptions).
//
// STATUS (2026-06-15): ALL 8 J3D2 BMD blocks implemented & verified — file
// header + block table + INF1 + VTX1 + EVP1 + DRW1 + JNT1 + SHP1 + MAT3 + TEX1
// (synthetic bmd_swap_test AND 15 real BMDs via scratch/bmd/verify_real, every
// file reaching all_covered==true with sane swapped values). Notes:
//   - EVP1's inverse-bind matrix count comes from JNT1's joint count via a
//     block-table counts pre-pass.
//   - VTX1 swaps the fmt list then each per-attr array as a homogeneous scalar
//     run (width from the fmt type — handles both f32 and s16 positions).
//   - SHP1 swaps all structural fields but DEFERS the display-list byte-stream
//     interior (BE u16 counts/indices) to the render path (VCD-driven parse).
//   - TEX1 swaps ResTIMG header scalars; palette/texel pixel data stays GC-native
//     (decoded by the port texture loader).
//   - MAT3 swaps every load-time-read table (init-data u16 indices, matID,
//     cull/texNo, texMtx/fog/nbtScale f32, tevColor s16, indInit f32); lightInfo
//     is empty in all SMS BMDs and its decomp layout is undefined (flagged, not
//     mis-swapped, if a non-empty span is ever hit).
// Next: run the real port loader (J3DModelLoader_v26::load) on a fully-swapped
// real BMD and confirm a non-null J3DModelData with sane fields (the flip gate).
// Do NOT wire this into the loader bridge until ALL blocks a given test model uses
// are covered, or the loader will crash on un-swapped interior data.
// =============================================================================
#ifndef SMSPORT_ASSETS_BMD_SWAP_H
#define SMSPORT_ASSETS_BMD_SWAP_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace smsport::assets {

// Which BMD blocks are fully byteswapped so far. Until this is COMPLETE for a
// model, the swapped buffer is unsafe to hand to the decomp loader.
struct BmdSwapResult {
	bool        ok            = false;  // header+block-table parsed, all present blocks dispatched
	bool        all_covered   = false;  // every present block had a real (non-stub) swapper
	uint32_t    block_num     = 0;
	uint32_t    blocks_covered = 0;     // count with a real swapper
	const char* error         = nullptr;
};

// Swap a big-endian J3D2 BMD (`be_data`, `len`) into a host-endian copy `out`.
// `out` is resized to `len`. Returns the coverage result (check .all_covered
// before feeding `out` to the loader).
BmdSwapResult bmd_swap_to_host(const uint8_t* be_data, size_t len,
                               std::vector<uint8_t>& out);

// Swap a big-endian J3D2 BMT (.bmt material table — a J3D2 container holding the
// same MAT3 + TEX1 blocks a BMD does, just no geometry) into a host-endian copy
// `out`. Reuses the BMD MAT3/TEX1 block swappers. Same contract as
// bmd_swap_to_host: check .all_covered before feeding `out` to
// J3DModelLoaderDataBase::loadMaterialTable.
BmdSwapResult bmt_swap_to_host(const uint8_t* be_data, size_t len,
                               std::vector<uint8_t>& out);

}  // namespace smsport::assets

#endif  // SMSPORT_ASSETS_BMD_SWAP_H
