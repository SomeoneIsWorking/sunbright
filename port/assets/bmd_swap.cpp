#include "bmd_swap.h"
#include "rarc.h"  // be16/be32 explicit big-endian reads

namespace smsport::assets {

// ---- in-place big-endian -> host swaps on the OUTPUT copy --------------------
// (the copy is byte-identical to the BE source, so reversing the bytes at an
// offset converts that field to host endianness).
static inline void sw16(uint8_t* p) {
	uint8_t t = p[0]; p[0] = p[1]; p[1] = t;
}
static inline void sw32(uint8_t* p) {
	uint8_t t;
	t = p[0]; p[0] = p[3]; p[3] = t;
	t = p[1]; p[1] = p[2]; p[2] = t;
}
// f32 has the same byte order as u32 — swapping the 4 bytes is correct.

// =============================================================================
// Per-block swappers. Each takes the OUTPUT base of the block and its size, plus
// the ORIGINAL big-endian block base (for structural reads via be16/be32 that
// must see un-swapped values). Offsets are relative to the block start; the
// JUTDataBlockHeader {u32 mType; u32 mSize} occupies +0x00..+0x08 and the tag +
// size are swapped by the caller.
// =============================================================================

// INF1 / J3DModelInfoBlock (reference J3DModelLoader.hpp):
//   +0x08 u16 mFlags
//   +0x0A u16 (unread padding / scaling-rule — left as-is)
//   +0x0C u32 mPacketNum
//   +0x10 u32 mVtxNum
//   +0x14 u32 mpHierarchy  (offset, relative to the block start)
//   [mpHierarchy] J3DModelHierarchy[] = {u16 mType; u16 mValue} until mType==0.
static void swap_INF1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x18) return;
	sw16(out + 0x08);          // mFlags
	sw32(out + 0x0C);          // mPacketNum
	sw32(out + 0x10);          // mVtxNum
	uint32_t hier = be32(be + 0x14);
	sw32(out + 0x14);          // mpHierarchy (offset)
	if (hier == 0 || hier >= size) return;
	// Walk the hierarchy entries until the (type==0) terminator.
	for (uint32_t o = hier; o + 4 <= size; o += 4) {
		uint16_t type = be16(be + o);
		sw16(out + o);         // mType
		sw16(out + o + 2);     // mValue
		if (type == 0) break;
	}
}

// DRW1 / J3DDrawBlock:
//   +0x08 u16 mMtxNum
//   +0x0C u32 mpDrawMtxFlag  (offset -> u8[mMtxNum], NO swap)
//   +0x10 u32 mpDrawMtxIndex (offset -> u16[mMtxNum], swap each)
static void swap_DRW1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x14) return;
	uint16_t mtx_num = be16(be + 0x08);
	uint32_t off_idx = be32(be + 0x10);
	sw16(out + 0x08);          // mMtxNum
	sw32(out + 0x0C);          // mpDrawMtxFlag (offset; the u8 array needs no swap)
	sw32(out + 0x10);          // mpDrawMtxIndex (offset)
	for (uint32_t i = 0; i < mtx_num; ++i) {
		uint32_t o = off_idx + i * 2;
		if (o + 2 > size) break;
		sw16(out + o);         // mDrawMtxIndex[i]
	}
}

// =============================================================================
BmdSwapResult bmd_swap_to_host(const uint8_t* be_data, size_t len,
                               std::vector<uint8_t>& out) {
	BmdSwapResult r;
	if (len < 0x24) { r.error = "too small"; return r; }
	out.assign(be_data, be_data + len);

	// File header: 'J3D2' magic, 'bmd3'/'bdl4' type, fileSize, blockNum.
	// (mSeAnmOffset @0x1C is BCK-only; not touched for BMD/BDL.)
	const uint32_t magic = be32(be_data + 0x00);
	if (magic != 0x4A334432 /* 'J3D2' */) { r.error = "not J3D2"; return r; }
	sw32(out.data() + 0x00);   // mMagic
	sw32(out.data() + 0x04);   // mType
	sw32(out.data() + 0x08);   // mFileSize
	sw32(out.data() + 0x0C);   // mBlockNum

	const uint32_t block_num = be32(be_data + 0x0C);
	r.block_num = block_num;

	uint32_t off = 0x20;  // mFirstBlock
	for (uint32_t i = 0; i < block_num; ++i) {
		if (off + 8 > len) { r.error = "block table overrun"; return r; }
		const uint32_t tag  = be32(be_data + off + 0);
		const uint32_t bsz  = be32(be_data + off + 4);
		if (bsz < 8 || off + bsz > len) { r.error = "bad block size"; return r; }
		sw32(out.data() + off + 0);   // mType (so the loader's fourcc compare matches)
		sw32(out.data() + off + 4);   // mSize

		uint8_t*       obo = out.data() + off;   // output block base
		const uint8_t* bbo = be_data + off;      // big-endian block base
		bool covered = true;
		switch (tag) {
		case 0x494E4631: /* INF1 */ swap_INF1(obo, bbo, bsz); break;
		case 0x44525731: /* DRW1 */ swap_DRW1(obo, bbo, bsz); break;
		// --- NOT YET IMPLEMENTED (field maps in the header doc) -------------
		// VTX1 (J3DVertexBlock): GXVtxAttrFmt list (attr/cnt/type u32 + frac u8)
		//   then per-attr arrays whose element layout (f32 pos/nrm, u8 color,
		//   f32 texcoord, ...) is driven by the fmt list. The hard one.
		// EVP1 (J3DEnvelopBlock): mWEvlpMtxNum u16 + 4 offsets; arrays of u8
		//   counts (no swap), u16 indices, f32 weights, Mtx (f32[3][4]) inv-binds.
		//   index/weight lengths = sum of the u8 counts (LOCAL); the inv-bind
		//   matrix count = JOINT count from JNT1 -> needs a counts pre-pass
		//   (cross-block dependency; blocks can precede JNT1).
		// JNT1: joint count u16 + offsets; per-joint matrix-type u16, flags, scale
		//   f32[3], rotation s16[3], translation f32[3], bbox f32.
		// SHP1: shape descriptors (mtx-type/count u16, display-list offset/size)
		//   + the GX display lists (byte streams — partial: swap descriptors only).
		// MAT3: material entries — large; many u16/u8 index tables + color/reg data.
		// TEX1 (J3DTextureBlock): mTextureNum u16 + offsets; ResTIMG headers
		//   (format u8, width/height u16, ...) + palette/texel byte streams.
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
