// =============================================================================
// Big-endian -> host byteswap for J3D BMD/BDL model files (the BE asset layer,
// Path A of docs/re_notes/first_flip_endianness.md).
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
// STATUS (2026-06-15): file header + block table + INF1 + DRW1 + JNT1 + EVP1 +
// VTX1 implemented & verified (synthetic bmd_swap_test AND real BMDs via
// scratch/bmd/verify_real). EVP1's inverse-bind matrix count comes from JNT1's
// joint count via a counts pre-pass; VTX1 swaps the fmt list then each per-attr
// array as a homogeneous scalar run (width from the fmt type — handles both f32
// and s16 positions). The remaining blocks (SHP1/MAT3/TEX1) are stubbed — see
// bmd_swap.cpp for the per-block field-map plan.
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

}  // namespace smsport::assets

#endif  // SMSPORT_ASSETS_BMD_SWAP_H
