// =============================================================================
// Big-endian -> host byteswap for J3D BMT (.bmt) material-table files.
//
// A .bmt is a J3D2 container holding the SAME MAT3 + TEX1 blocks a .bmd does, just
// without the geometry blocks (it overrides a model's materials/textures — e.g.
// the shared map-object material tables /scene/mapObj/*.bmt loaded by
// TMapObjManager::loadMatTable). The decomp loader
// (J3DModelLoaderDataBase::loadMaterialTable) reads the file by raw struct cast,
// so on a little-endian host the 'J3D2' fourcc and every field are misread and the
// load returns nullptr -> a null J3DMaterialTable deref in setMaterialTable (the
// TWoodBarrel/initUnique crash). Mirror bmd_swap_to_host: produce a host-endian
// copy at load, reusing the BMD MAT3/TEX1 block swappers (bmd_blocks.h forwarders).
//
// Portable across x86-64 and arm64 (explicit be16/be32, no host-byte-order
// assumptions). See bmd_swap.cpp for the per-table layout documentation.
// =============================================================================
#include "bmd_swap.h"
#include "bmd_blocks.h"
#include <cstdio>
#include <cstdlib>

namespace smsport::assets {

namespace {
uint32_t be32(const uint8_t* p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8)
	       | p[3];
}
void sw32(uint8_t* p) {
	uint8_t t;
	t = p[0]; p[0] = p[3]; p[3] = t;
	t = p[1]; p[1] = p[2]; p[2] = t;
}
}  // namespace

BmdSwapResult bmt_swap_to_host(const uint8_t* be_data, size_t len,
                               std::vector<uint8_t>& out) {
	BmdSwapResult r;
	if (len < 0x24) { r.error = "too small"; return r; }
	out.assign(be_data, be_data + len);

	// File header: 'J3D2' magic, 'bmt3'/'bmt2' type, fileSize @0x8, blockNum @0xC,
	// mFirstBlock @0x20 (0x10..0x1F = subversion string, byte data — left as-is).
	const uint32_t magic = be32(be_data + 0x00);
	if (magic != 0x4A334432 /* 'J3D2' */) { r.error = "not J3D2"; return r; }
	sw32(out.data() + 0x00);   // mMagic
	sw32(out.data() + 0x04);   // mType  (so the loader's bmt2/bmt3 fourcc compare matches)
	sw32(out.data() + 0x08);   // mFileSize
	sw32(out.data() + 0x0C);   // mBlockNum

	const uint32_t block_num = be32(be_data + 0x0C);
	r.block_num = block_num;

	const bool dbg = getenv("SB_BMT_DBG") != nullptr;
	if (dbg)
		std::fprintf(stderr,
		             "[bmt] len(arg)=%zu mFileSize=%u blockNum=%u type=%c%c%c%c\n",
		             len, be32(be_data + 0x08), block_num, be_data[4], be_data[5],
		             be_data[6], be_data[7]);

	// Some SMS .bmt files (e.g. nozzleBox.bmt) declare mBlockNum larger than the
	// number of blocks that physically fit — the last real block ends exactly at
	// EOF and the remaining declared block(s) are phantoms with no in-file bytes.
	// The decomp loader tolerates this (it reads the phantom header past EOF, hits
	// the `default` case for a zero/garbage tag and the block pointer doesn't
	// advance). Mirror that tolerance: stop when the file bytes are exhausted, and
	// gauge coverage against the blocks actually PRESENT (not the declared count).
	uint32_t off     = 0x20;  // mFirstBlock
	uint32_t present = 0;
	for (uint32_t i = 0; i < block_num; ++i) {
		if (off >= len) break;  // remaining declared blocks are phantom
		if (off + 8 > len) { r.error = "block header overrun"; return r; }
		const uint32_t tag = be32(be_data + off + 0);
		const uint32_t bsz = be32(be_data + off + 4);
		if (dbg)
			std::fprintf(stderr, "[bmt]  block %u @0x%x tag=%c%c%c%c size=%u\n", i,
			             off, be_data[off], be_data[off + 1], be_data[off + 2],
			             be_data[off + 3], bsz);
		if (bsz < 8 || off + bsz > len) { r.error = "bad block size"; return r; }
		sw32(out.data() + off + 0);   // mType
		sw32(out.data() + off + 4);   // mSize

		uint8_t*       obo = out.data() + off;   // output block base
		const uint8_t* bbo = be_data + off;      // big-endian block base
		bool covered = true;
		switch (tag) {
		case 0x4D415433: /* MAT3 */ detail::swap_MAT3_block(obo, bbo, bsz); break;
		case 0x54455831: /* TEX1 */ detail::swap_TEX1_block(obo, bbo, bsz); break;
		// MAT2 (J3D2 bmt2 / v21) is not used by the SMS map-object .bmt files
		// (all bmt3 / MAT3). Leave it uncovered so all_covered==false fails fast
		// rather than feeding the loader a half-swapped v21 table.
		default: covered = false; break;
		}
		++present;
		if (covered) r.blocks_covered++;
		off += bsz;
	}

	r.ok = true;
	r.all_covered = (r.blocks_covered == present);
	return r;
}

}  // namespace smsport::assets
