#include "bmt_swap.h"
#include "bmd_blocks.h"  // detail::swap_MAT3_block / swap_TEX1_block (shared)
#include "rarc.h"        // be32 explicit big-endian read

namespace smsport::assets {

// In-place big-endian -> host swap of one u32 in the output copy.
static inline void sw32(uint8_t* p) {
	uint8_t t;
	t = p[0]; p[0] = p[3]; p[3] = t;
	t = p[1]; p[1] = p[2]; p[2] = t;
}

BmtSwapResult bmt_swap_to_host(const uint8_t* be_data, size_t len,
                               std::vector<uint8_t>& out) {
	BmtSwapResult r;
	if (len < 0x24) { r.error = "too small"; return r; }
	out.assign(be_data, be_data + len);

	// File header (JUTDataFileHeader): 'J3D2' magic, 'bmt2'/'bmt3' type, fileSize,
	// blockNum; mFirstBlock @ 0x20 (same 0x20 header as a BMD).
	const uint32_t magic = be32(be_data + 0x00);
	if (magic != 0x4A334432 /* 'J3D2' */) { r.error = "not J3D2"; return r; }
	const uint32_t type = be32(be_data + 0x04);
	if (type != 0x626D7433 /* 'bmt3' */ && type != 0x626D7432 /* 'bmt2' */) {
		r.error = "not bmt2/bmt3";
		return r;
	}
	sw32(out.data() + 0x00);   // mMagic
	sw32(out.data() + 0x04);   // mType
	sw32(out.data() + 0x08);   // mFileSize
	sw32(out.data() + 0x0C);   // mBlockNum

	const uint32_t block_num = be32(be_data + 0x0C);
	r.block_num = block_num;

	uint32_t off = 0x20;  // mFirstBlock
	for (uint32_t i = 0; i < block_num; ++i) {
		if (off + 8 > len) { r.error = "block table overrun"; return r; }
		const uint32_t tag = be32(be_data + off + 0);
		const uint32_t bsz = be32(be_data + off + 4);
		if (bsz < 8 || off + bsz > len) { r.error = "bad block size"; return r; }
		sw32(out.data() + off + 0);   // mType (so the loader's fourcc compare matches)
		sw32(out.data() + off + 4);   // mSize

		uint8_t*       obo = out.data() + off;   // output block base
		const uint8_t* bbo = be_data + off;      // big-endian block base
		bool covered = true;
		switch (tag) {
		case 0x4D415433: /* MAT3 */ detail::swap_MAT3_block(obo, bbo, bsz); break;
		case 0x54455831: /* TEX1 */ detail::swap_TEX1_block(obo, bbo, bsz); break;
		// MAT2 (bmt2) / MDL3 not covered yet — none seen in SMS .bmt. A non-empty
		// span here surfaces loud via all_covered==false (the loader gate rejects it).
		default: covered = false; break;
		}
		if (covered) r.blocks_covered++;
		off += bsz;
	}

	r.ok = true;
	r.all_covered = (r.blocks_covered == block_num);
	return r;
}

}  // namespace smsport::assets
