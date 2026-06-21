// =============================================================================
// Big-endian -> host byteswap for J3D animation files (.bck/.btp/.btk/...), the
// BE asset layer's sibling to bmd_swap.cpp (J3D1 container instead of J3D2).
//
// On-disc J3D1 animation files are big-endian. The decomp loader
// (J3DAnmLoaderDataBase::load -> J3DAnm{Key,Full}Loader_v15) reads every multibyte
// field (incl. the 'J3D1' fourcc magic and the 'ANK1'/'TPT1'/... block tags it
// compares as host u32 constants) by raw struct cast, so on a little-endian host
// the magic check fails and load() returns null — then loadAnmTexPattern derefs
// the null (MarioDraw.cpp). This module produces a HOST-ENDIAN copy at load,
// keeping the pristine decomp loader untouched ("swap at load"), modeled exactly
// on bmd_swap_to_host.
//
// Strategy (identical to bmd_swap): copy the BE buffer, walk the ORIGINAL BE bytes
// (explicit be16/be32, never struct overlay) to learn structure, swap each
// multibyte field IN PLACE in the copy. Homogeneous value arrays (f32 scale/trans,
// s16 rot) are swapped as uniform scalar runs over [offset, nextBoundary). u8
// arrays (visibility) stay as-is.
//
// STATUS (2026-06-21): ANK1 (bck transform-key) + TPT1 (btp texpattern-full) — the
// two formats TMario::initModel loads NOW — are implemented & unit-tested
// (anm_swap_test). The remaining 10 block types (TTK1/TRK1/PAK1/PAF1/ANF1/VAF1/
// CLF1/VCF1/VCK1/CLK1) are added as later actors hit them; anm_swap_to_host reports
// all_covered=false on any present-but-unhandled block so the loader OSPanics LOUD
// (FAIL FAST) instead of feeding the loader half-swapped data.
//
// Portable across x86-64 and arm64 (no host-byte-order assumptions).
// =============================================================================
#ifndef SMSPORT_ASSETS_ANM_SWAP_H
#define SMSPORT_ASSETS_ANM_SWAP_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace smsport::assets {

// Which animation blocks are fully byteswapped so far. Until all_covered is true
// for a file, the swapped buffer is unsafe to hand to the decomp loader.
struct AnmSwapResult {
	bool        ok             = false;  // header parsed, all present blocks dispatched
	bool        all_covered    = false;  // every present block had a real (non-stub) swapper
	uint32_t    block_num      = 0;
	uint32_t    blocks_covered = 0;      // count with a real swapper
	const char* error          = nullptr;
};

// Swap a big-endian J3D1 animation file (`be_data`, `len`) into a host-endian copy
// `out` (resized to `len`). Returns the coverage result (check .all_covered before
// feeding `out` to the loader).
AnmSwapResult anm_swap_to_host(const uint8_t* be_data, size_t len,
                               std::vector<uint8_t>& out);

}  // namespace smsport::assets

#endif  // SMSPORT_ASSETS_ANM_SWAP_H
