// =============================================================================
// Big-endian -> host byteswap for J3D animation files (.bck/.btk/.brk/.bpk/.blk/
// .bxk + the *Full variants .bca/.bpa/.btp/.bla/.bva/.bxa).
//
// An animation file is a J3D1 container ('bck1'/'btk1'/...) holding one animation
// block (ANK1/TTK1/TRK1/PAK1/CLK1/VCK1 for the KEY family, ANF1/PAF1/TPT1/CLF1/
// VAF1/VCF1 for the FULL family) that J3DAnmLoaderDataBase::load reads into a
// J3DAnmBase subclass. Same "swap at load" strategy as bmd_swap/bmt_swap: copy
// the BE buffer, walk the ORIGINAL bytes to learn structure, swap each multibyte
// field in place in the copy. Portable across x86-64 and arm64.
//
// CRITICAL: this swapper covers the WHOLE J3D1 block family. The loader bridge
// fires for EVERY animation load on the boot path (many actors load .bck before
// any .btk), so a PARTIAL swapper would silently corrupt animators it doesn't
// cover — WORSE than a clean fault. Unknown/uncovered blocks set all_covered=false
// so the bridge can refuse the buffer rather than feed half-swapped data.
//
// The block element layouts mirror J3DAnmLoader.cpp's readAnm* readers exactly:
// keyframe TABLES are u16 runs (J3DAnmKeyTableBase = 3xu16), value arrays are f32
// (scale/trans/weight/SRTCenter), s16 (rotation, KEY-family color/tevreg) or u8
// (updateTexMtxID, FULL-family color values — NO swap); name tables go through the
// shared ResNTAB swapper. See anm_swap.cpp for the per-block documentation.
// =============================================================================
#ifndef SMSPORT_ASSETS_ANM_SWAP_H
#define SMSPORT_ASSETS_ANM_SWAP_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace smsport::assets {

struct AnmSwapResult {
	bool        ok             = false;  // header parsed, block dispatched
	bool        all_covered    = false;  // the present block had a real swapper
	uint32_t    block_num      = 0;
	uint32_t    blocks_covered = 0;      // count with a real swapper
	uint32_t    file_type      = 0;      // J3D1 sub-type fourcc (e.g. 'bck1')
	const char* error          = nullptr;
};

// Swap a big-endian J3D1 animation file (`be_data`, `len`) into a host-endian copy
// `out` (resized to `len`). Check `.all_covered` before feeding `out` to the loader
// (J3DAnmLoaderDataBase::load).
AnmSwapResult anm_swap_to_host(const uint8_t* be_data, size_t len,
                               std::vector<uint8_t>& out);

}  // namespace smsport::assets

#endif  // SMSPORT_ASSETS_ANM_SWAP_H
