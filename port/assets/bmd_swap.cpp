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

// ResNTAB (JUTNameTab.hpp): u16 mEntryNum; u16 mPad0; Entry{u16 mKeyCode; u16
// mOffs}[mEntryNum]; then a packed string blob (left untouched). `off` is the
// block-relative offset of the table.
static void swap_ResNTAB(uint8_t* out, const uint8_t* be, uint32_t off,
                         uint32_t size) {
	if (off == 0 || off + 4 > size) return;
	uint16_t count = be16(be + off);
	sw16(out + off);               // mEntryNum
	// out+off+2 mPad0 — leave
	for (uint32_t i = 0; i < count; ++i) {
		uint32_t e = off + 4 + i * 4;
		if (e + 4 > size) break;
		sw16(out + e);             // mKeyCode (hash)
		sw16(out + e + 2);         // mOffs (string offset)
	}
	// string blob after the entries: byte data, no swap.
}

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

// JNT1 / J3DJointBlock (J3DJointFactory.hpp):
//   +0x08 u16 mJointNum
//   +0x0A u16 pad
//   +0x0C u32 mpJointInitData (offset -> J3DJointInitData[mJointNum])
//   +0x10 u32 mpIndexTable    (offset -> u16[mJointNum])
//   +0x14 u32 mpNameTable     (offset -> ResNTAB, or 0)
// J3DJointInitData has a REAL stride of 0x40 (the header's "Size: 0x30"/mMax@0x2C
// annotation is wrong — verified across real BMDs: (idxOff-initOff)/jointNum==0x40
// for every file). Layout @stride 0x40:
//   +0x00 u16 mKind
//   +0x02 u8  mScaleCompensate (+0x03 pad)        — no swap
//   +0x04 J3DTransformInfo (0x20): Vec mScale (f32x3) @+0x00, S16Vec mRotation
//         (s16x3 @+0x0C, +0x12 pad), Vec mTranslate (f32x3) @+0x14
//   +0x24 f32 mRadius
//   +0x28 Vec mMin (f32x3)
//   +0x34 Vec mMax (f32x3)   [ends 0x40]
static void swap_JNT1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x18) return;
	uint16_t joint_num = be16(be + 0x08);
	uint32_t off_init  = be32(be + 0x0C);
	uint32_t off_idx   = be32(be + 0x10);
	uint32_t off_name  = be32(be + 0x14);
	sw16(out + 0x08);              // mJointNum
	sw32(out + 0x0C);              // mpJointInitData (offset)
	sw32(out + 0x10);              // mpIndexTable (offset)
	sw32(out + 0x14);              // mpNameTable (offset)
	if (off_init != 0) {
		for (uint32_t i = 0; i < joint_num; ++i) {
			uint32_t b = off_init + i * 0x40;
			if (b + 0x40 > size) break;
			sw16(out + b + 0x00);          // mKind
			// b+0x02 mScaleCompensate u8 + b+0x03 pad: no swap
			uint32_t t = b + 0x04;         // J3DTransformInfo
			sw32(out + t + 0x00); sw32(out + t + 0x04); sw32(out + t + 0x08); // mScale
			sw16(out + t + 0x0C); sw16(out + t + 0x0E); sw16(out + t + 0x10); // mRotation
			// t+0x12 pad
			sw32(out + t + 0x14); sw32(out + t + 0x18); sw32(out + t + 0x1C); // mTranslate
			sw32(out + b + 0x24);          // mRadius
			sw32(out + b + 0x28); sw32(out + b + 0x2C); sw32(out + b + 0x30); // mMin
			sw32(out + b + 0x34); sw32(out + b + 0x38); sw32(out + b + 0x3C); // mMax
		}
	}
	if (off_idx != 0) {
		for (uint32_t i = 0; i < joint_num; ++i) {
			uint32_t o = off_idx + i * 2;
			if (o + 2 > size) break;
			sw16(out + o);                 // mpIndexTable[i]
		}
	}
	swap_ResNTAB(out, be, off_name, size);
}

// EVP1 / J3DEnvelopBlock (J3DModelLoader.hpp):
//   +0x08 u16 mWEvlpMtxNum
//   +0x0C u32 mpWEvlpMixMtxNum   (offset -> u8[mWEvlpMtxNum], NO swap)
//   +0x10 u32 mpWEvlpMixMtxIndex (offset -> u16[Sumcounts], swap)
//   +0x14 u32 mpWEvlpMixWeight   (offset -> f32[Sumcounts], swap)
//   +0x18 u32 mpInvJointMtx      (offset -> Mtx=f32[3][4] per JOINT, swap)
// Sumcounts = sum of the u8 mpWEvlpMixMtxNum array (LOCAL to this block). The
// inverse-bind matrix count is the JNT1 joint count -> cross-block dependency,
// passed in as `joint_num`. When mWEvlpMtxNum==0 (no skinning) all offsets are 0.
static void swap_EVP1(uint8_t* out, const uint8_t* be, uint32_t size,
                      uint16_t joint_num) {
	if (size < 0x1C) return;
	uint16_t n       = be16(be + 0x08);
	uint32_t off_num = be32(be + 0x0C);
	uint32_t off_idx = be32(be + 0x10);
	uint32_t off_w   = be32(be + 0x14);
	uint32_t off_inv = be32(be + 0x18);
	sw16(out + 0x08);                                        // mWEvlpMtxNum
	sw32(out + 0x0C); sw32(out + 0x10);
	sw32(out + 0x14); sw32(out + 0x18);                     // the 4 offsets
	// Sumcounts = sum of the u8 per-vertex-matrix counts (no swap on the u8s).
	uint32_t sum = 0;
	if (off_num != 0) {
		for (uint32_t i = 0; i < n; ++i) {
			if (off_num + i >= size) break;
			sum += be[off_num + i];
		}
	}
	if (off_idx != 0) {
		for (uint32_t i = 0; i < sum; ++i) {
			uint32_t o = off_idx + i * 2;
			if (o + 2 > size) break;
			sw16(out + o);                  // mpWEvlpMixMtxIndex[i]
		}
	}
	if (off_w != 0) {
		for (uint32_t i = 0; i < sum; ++i) {
			uint32_t o = off_w + i * 4;
			if (o + 4 > size) break;
			sw32(out + o);                  // mpWEvlpMixWeight[i]
		}
	}
	if (off_inv != 0) {
		for (uint32_t j = 0; j < joint_num; ++j) {
			for (uint32_t k = 0; k < 12; ++k) {   // Mtx = f32[3][4]
				uint32_t o = off_inv + (j * 12 + k) * 4;
				if (o + 4 > size) break;
				sw32(out + o);
			}
		}
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

	// Counts pre-pass: EVP1's inverse-bind matrix count is JNT1's joint count,
	// and EVP1 precedes JNT1 in the block order — so resolve the joint count by
	// scanning the block table for JNT1 (reading its big-endian mJointNum at
	// block+0x08) before swapping any block.
	uint16_t joint_num = 0;
	{
		uint32_t po = 0x20;
		for (uint32_t i = 0; i < block_num; ++i) {
			if (po + 8 > len) break;
			const uint32_t ptag = be32(be_data + po + 0);
			const uint32_t psz  = be32(be_data + po + 4);
			if (psz < 8 || po + psz > len) break;
			if (ptag == 0x4A4E5431 /* JNT1 */ && po + 0x0A <= len)
				joint_num = be16(be_data + po + 0x08);
			po += psz;
		}
	}

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
		case 0x4A4E5431: /* JNT1 */ swap_JNT1(obo, bbo, bsz); break;
		case 0x45565031: /* EVP1 */ swap_EVP1(obo, bbo, bsz, joint_num); break;
		// --- NOT YET IMPLEMENTED (field maps in the header doc) -------------
		// VTX1 (J3DVertexBlock): GXVtxAttrFmt list (attr/cnt/type u32 + frac u8)
		//   then per-attr arrays whose element layout (f32 pos/nrm, u8 color,
		//   f32 texcoord, ...) is driven by the fmt list. The hard one.
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
