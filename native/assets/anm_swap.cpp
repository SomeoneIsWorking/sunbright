#include "anm_swap.h"
#include "bmd_blocks.h"  // detail::swap_ResNTAB_block (shared name-table swapper)
#include "rarc.h"        // be16/be32 explicit big-endian reads

namespace smsport::assets {

// ---- in-place big-endian -> host swaps on the OUTPUT copy --------------------
// (the copy is byte-identical to the BE source, so reversing the bytes at an
// offset converts that field to host endianness). f32 shares u32's byte order.
static inline void sw16(uint8_t* p) {
	uint8_t t = p[0]; p[0] = p[1]; p[1] = t;
}
static inline void sw32(uint8_t* p) {
	uint8_t t;
	t = p[0]; p[0] = p[3]; p[3] = t;
	t = p[1]; p[1] = p[2]; p[2] = t;
}
// Swap a contiguous homogeneous run [start,end) of `unit`-byte scalars.
static void swap_run(uint8_t* out, uint32_t start, uint32_t end, uint32_t unit) {
	if (unit < 2) return;
	for (uint32_t o = start; o + unit <= end; o += unit) {
		if (unit == 2) sw16(out + o);
		else if (unit == 4) sw32(out + o);
	}
}

// Region-end resolver: given a sorted set of region-start offsets + the block
// size, the end of the region starting at `start` is the next-greater boundary.
static uint32_t region_end(const uint32_t* bounds, int nb, uint32_t start,
                           uint32_t size) {
	uint32_t end = size;
	for (int j = 0; j < nb; ++j)
		if (bounds[j] > start && bounds[j] < end) end = bounds[j];
	return end;
}
static void sort_bounds(uint32_t* b, int n) {
	for (int i = 0; i < n; ++i)
		for (int j = i + 1; j < n; ++j)
			if (b[j] < b[i]) { uint32_t t = b[i]; b[i] = b[j]; b[j] = t; }
}

// =============================================================================
// Per-block swappers. `out` = host-endian COPY's block base (the block header
// tag+size are already swapped by the caller), `be` = ORIGINAL big-endian block
// base (read for structure), `size` = block size. The JUTDataBlockHeader
// {u32 mType; u32 mSize} occupies +0x00..+0x08.
// =============================================================================

// ANK1 / J3DAnmTransformKeyData (.bck transform KEY) — J3DAnmKeyLoader_v15::
// readAnmTransform (J3DAnmLoader.cpp:264). Header layout (J3DAnmLoader.hpp):
//   +0x08 u8  mAttribute (+0x09 u8 mDecShift): no swap
//   +0x0A s16 mFrameMax
//   +0x0C u16 field_0xc  (= track count -> field_0x22)
//   +0x10 s32 field_0x10 (unread; swapped for completeness)
//   +0x14 s32 mTableOffset (-> J3DAnmTransformKeyTable[count], stride 0x12)
//   +0x18 s32 mScaleOffset (-> f32[])
//   +0x1C s32 mRotOffset   (-> s16[])
//   +0x20 s32 mTransOffset (-> f32[])
// J3DAnmTransformKeyTable = 3x J3DAnmKeyTableBase{u16 mMaxFrame; u16 mOffset; u16
// mType} = NINE u16, stride 0x12, fully homogeneous -> swap as a u16 run.
static void swap_ANK1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x24) return;
	uint16_t count     = be16(be + 0x0C);
	uint32_t off_tab   = be32(be + 0x14);
	uint32_t off_scale = be32(be + 0x18);
	uint32_t off_rot   = be32(be + 0x1C);
	uint32_t off_trans = be32(be + 0x20);
	sw16(out + 0x0A);                 // mFrameMax
	sw16(out + 0x0C);                 // field_0xc (count)
	sw32(out + 0x10);                 // field_0x10
	sw32(out + 0x14); sw32(out + 0x18); sw32(out + 0x1C); sw32(out + 0x20);
	// Transform key table: count entries x 9 u16 (stride 0x12), homogeneous.
	if (off_tab != 0)
		swap_run(out, off_tab, off_tab + (uint32_t)count * 0x12 <= size
		                           ? off_tab + (uint32_t)count * 0x12 : size, 2);
	// Value arrays sized by region boundary (count of keys is not in the header).
	uint32_t bounds[5]; int nb = 0;
	if (off_tab)   bounds[nb++] = off_tab;
	if (off_scale) bounds[nb++] = off_scale;
	if (off_rot)   bounds[nb++] = off_rot;
	if (off_trans) bounds[nb++] = off_trans;
	bounds[nb++] = size;
	sort_bounds(bounds, nb);
	if (off_scale) swap_run(out, off_scale, region_end(bounds, nb, off_scale, size), 4);
	if (off_rot)   swap_run(out, off_rot,   region_end(bounds, nb, off_rot,   size), 2);
	if (off_trans) swap_run(out, off_trans, region_end(bounds, nb, off_trans, size), 4);
}

// TPT1 / J3DAnmTexPatternFullData (.btp texpattern FULL) — J3DAnmFullLoader_v15::
// readAnmTexPattern (J3DAnmLoader.cpp:152). Header layout (J3DAnmLoader.hpp):
//   +0x08 u8 field_0x8 (+0x09 u8 field_0x9): no swap
//   +0x0A s16 mFrameMax
//   +0x0C u16 field_0xc  (= mUpdateMaterialNum -> table & matID count)
//   +0x0E u16 field_0xe  (-> dst->field_0x18)
//   +0x10 s32 mTableOffset            (-> J3DAnmTexPatternFullTable[count])
//   +0x14 s32 mValuesOffset           (-> u16[] texture indices)
//   +0x18 s32 mUpdateMaterialIDOffset (-> u16[count])
//   +0x1C s32 mNameTabOffset          (-> ResNTAB)
// J3DAnmTexPatternFullTable (stride 0x8): u16 mMaxFrame@0x00, u16 mOffset@0x02,
// u8 mTexNo@0x04 (+0x05 pad), u16 _6@0x06 — NOT homogeneous (u8 hole) -> per-entry.
static void swap_TPT1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x20) return;
	uint16_t count     = be16(be + 0x0C);
	uint32_t off_tab   = be32(be + 0x10);
	uint32_t off_vals  = be32(be + 0x14);
	uint32_t off_matid = be32(be + 0x18);
	uint32_t off_name  = be32(be + 0x1C);
	sw16(out + 0x0A);                 // mFrameMax
	sw16(out + 0x0C);                 // field_0xc (count)
	sw16(out + 0x0E);                 // field_0xe
	sw32(out + 0x10); sw32(out + 0x14); sw32(out + 0x18); sw32(out + 0x1C);
	// Texpattern table: count entries, stride 8, u16@0/u16@2/(u8@4)/u16@6.
	if (off_tab != 0) {
		for (uint32_t i = 0; i < count; ++i) {
			uint32_t b = off_tab + i * 8;
			if (b + 8 > size) break;
			sw16(out + b + 0x00);    // mMaxFrame
			sw16(out + b + 0x02);    // mOffset
			// b+0x04 mTexNo u8 + b+0x05 pad: no swap
			sw16(out + b + 0x06);    // _6
		}
	}
	// Homogeneous u16 arrays sized by region boundary.
	uint32_t bounds[5]; int nb = 0;
	if (off_tab)   bounds[nb++] = off_tab;
	if (off_vals)  bounds[nb++] = off_vals;
	if (off_matid) bounds[nb++] = off_matid;
	if (off_name)  bounds[nb++] = off_name;
	bounds[nb++] = size;
	sort_bounds(bounds, nb);
	if (off_vals)  swap_run(out, off_vals,  region_end(bounds, nb, off_vals,  size), 2);
	if (off_matid) swap_run(out, off_matid, region_end(bounds, nb, off_matid, size), 2);
	detail::swap_ResNTAB_block(out, be, off_name, size);
}

// =============================================================================
AnmSwapResult anm_swap_to_host(const uint8_t* be_data, size_t len,
                               std::vector<uint8_t>& out) {
	AnmSwapResult r;
	if (len < 0x28) { r.error = "too small"; return r; }
	out.assign(be_data, be_data + len);

	// File header (JUTDataFileHeader): 'J3D1' magic, 'bck1'/'btp1'/... type,
	// fileSize, blockNum. mSeAnmOffset @0x1C is BCK-only and never read by the
	// loader (verified: no src/ reference) — left untouched.
	const uint32_t magic = be32(be_data + 0x00);
	if (magic != 0x4A334431 /* 'J3D1' */) { r.error = "not J3D1"; return r; }
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

		uint8_t*       obo = out.data() + off;
		const uint8_t* bbo = be_data + off;
		bool covered = true;
		switch (tag) {
		case 0x414E4B31: /* ANK1 */ swap_ANK1(obo, bbo, bsz); break;
		case 0x54505431: /* TPT1 */ swap_TPT1(obo, bbo, bsz); break;
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
