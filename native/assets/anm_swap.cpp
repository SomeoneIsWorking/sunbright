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

// TTK1 / J3DAnmTextureSRTKeyData (.btk texture-SRT KEY) — J3DAnmKeyLoader_v15::
// readAnmTextureSRT (J3DAnmLoader.cpp:281). Header (J3DAnmLoader.hpp:69):
//   +0x08 u8 mAttribute (+0x09 u8 mDecShift): no swap
//   +0x0A s16 mMaxFrame, +0x0C u16 mTrackNum, +0x0E u16 mScaleNum,
//   +0x10 u16 mRotNum, +0x12 u16 mTransNum
//   +0x14 s32 mTableOffset           (-> J3DAnmTransformKeyTable[mTrackNum], all u16)
//   +0x18 s32 mUpdateMatIDOffset      (-> u16[])
//   +0x1C s32 mNameTab1Offset         (-> ResNTAB)
//   +0x20 s32 mUpdateTexMtxIDOffset   (-> u8[],   NO swap)
//   +0x24 s32 unkOffset (SRTCenter)   (-> Vec/f32[])
//   +0x28 s32 mScaleValOffset         (-> f32[])
//   +0x2C s32 mRotValOffset           (-> s16[])
//   +0x30 s32 mTransValOffset         (-> f32[])
//   +0x34 u16 mPostTrackNum, +0x36/0x38/0x3A u16 (unk40/42/44)
//   +0x3C s32 mInfoTable2Offset       (-> J3DAnmTransformKeyTable[mPostTrackNum], u16)
//   +0x40 s32 PostUpdateMatIDOffset   (-> u16[])
//   +0x44 u32 mNameTab2Offset         (-> ResNTAB, or 0)
//   +0x48 s32 PostUpdateTexMtxIDOff   (-> u8[],   NO swap)
//   +0x4C s32 PostSRTCenterOffset     (-> Vec/f32[])
//   +0x50 s32 unk48Offset             (-> f32[])
//   +0x54 s32 unk4COffset             (-> s16[])
//   +0x58 s32 unk50Offset             (-> f32[])
//   +0x5C s32 field_0x5c (unread)
// Each offset-referenced array is a homogeneous scalar run; widths above. Element
// counts aren't all in the header, so each runs to the next offset boundary.
static void swap_TTK1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x60) return;
	// Header count fields.
	sw16(out + 0x0A); sw16(out + 0x0C); sw16(out + 0x0E);
	sw16(out + 0x10); sw16(out + 0x12);
	sw16(out + 0x34); sw16(out + 0x36); sw16(out + 0x38); sw16(out + 0x3A);
	// All offset words 0x14..0x5C (s32/u32).
	for (uint32_t o = 0x14; o <= 0x5C; o += 4) sw32(out + o);

	// Offset-referenced arrays: {block-relative offset, element width, kind}.
	// kind: 0 = numeric run (width), 1 = ResNTAB, 2 = u8 (no swap, boundary only).
	struct Arr { uint32_t off; uint32_t width; int kind; };
	Arr a[16];
	int n = 0;
	a[n++] = { be32(be + 0x14), 2, 0 };   // table u16
	a[n++] = { be32(be + 0x18), 2, 0 };   // updateMaterialID u16
	a[n++] = { be32(be + 0x1C), 0, 1 };   // nameTab1
	a[n++] = { be32(be + 0x20), 1, 2 };   // updateTexMtxID u8
	a[n++] = { be32(be + 0x24), 4, 0 };   // SRTCenter f32
	a[n++] = { be32(be + 0x28), 4, 0 };   // scale f32
	a[n++] = { be32(be + 0x2C), 2, 0 };   // rot s16
	a[n++] = { be32(be + 0x30), 4, 0 };   // trans f32
	a[n++] = { be32(be + 0x3C), 2, 0 };   // infoTable2 u16
	a[n++] = { be32(be + 0x40), 2, 0 };   // postUpdateMaterialID u16
	a[n++] = { be32(be + 0x44), 0, 1 };   // nameTab2
	a[n++] = { be32(be + 0x48), 1, 2 };   // postUpdateTexMtxID u8
	a[n++] = { be32(be + 0x4C), 4, 0 };   // postSRTCenter f32
	a[n++] = { be32(be + 0x50), 4, 0 };   // unk48 f32
	a[n++] = { be32(be + 0x54), 2, 0 };   // unk4C s16
	a[n++] = { be32(be + 0x58), 4, 0 };   // unk50 f32

	uint32_t bounds[17]; int nb = 0;
	for (int i = 0; i < n; ++i) if (a[i].off) bounds[nb++] = a[i].off;
	bounds[nb++] = size;
	sort_bounds(bounds, nb);
	for (int i = 0; i < n; ++i) {
		if (!a[i].off) continue;
		if (a[i].kind == 1) { detail::swap_ResNTAB_block(out, be, a[i].off, size); continue; }
		if (a[i].kind == 2) continue;     // u8 array: no swap
		swap_run(out, a[i].off, region_end(bounds, nb, a[i].off, size), a[i].width);
	}
}

// A J3DAnm{C,K}RegKeyTable (J3DAnimation.hpp): 4x J3DAnmKeyTableBase{u16 mMaxFrame;
// u16 mOffset; u16 mType} = 12 u16, then u8 mColorId@0x18 + pad[3]. Stride 0x1C.
static void swap_RegKeyTable(uint8_t* e) {
	for (int i = 0; i < 12; ++i) sw16(e + i * 2);   // 0x00..0x17 all u16
	// 0x18 mColorId u8 + 0x19..0x1B pad: no swap
}

// TRK1 / J3DAnmTevRegKeyData (.brk TEV-register KEY) — readAnmTevReg
// (J3DAnmLoader.cpp:366). Header (J3DAnmLoader.hpp:130, size 0x58):
//   +0x0A s16 mFrameMax; +0x0C u16 mCRegUpdateMaterialNum;
//   +0x0E u16 mKRegUpdateMaterialNum; +0x10..0x1E u16 x8 (CReg/KReg R/G/B/A counts)
//   +0x20 s32 mCRegTableOffset (-> J3DAnmCRegKeyTable[mCRegUpdateMaterialNum])
//   +0x24 s32 mKRegTableOffset (-> J3DAnmKRegKeyTable[mKRegUpdateMaterialNum])
//   +0x28/0x2C s32 C/K RegUpdateMaterialIDOffset (-> u16[])
//   +0x30/0x34 s32 C/K RegNameTabOffset (-> ResNTAB)
//   +0x38..0x44 s32 C R/G/B/A ValuesOffset (-> s16[])
//   +0x48..0x54 s32 K R/G/B/A ValuesOffset (-> s16[])
static void swap_TRK1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x58) return;
	uint16_t cregNum = be16(be + 0x0C);
	uint16_t kregNum = be16(be + 0x0E);
	sw16(out + 0x0A);                               // mFrameMax
	for (uint32_t o = 0x0C; o <= 0x1E; o += 2) sw16(out + o);   // 10 u16 counts
	uint32_t off[14];
	for (int i = 0; i < 14; ++i) { off[i] = be32(be + 0x20 + i * 4); sw32(out + 0x20 + i * 4); }
	// off: 0 CRegTable,1 KRegTable,2 CRegMatID,3 KRegMatID,4 CRegName,5 KRegName,
	//      6..9 CR/CG/CB/CA values,10..13 KR/KG/KB/KA values.
	if (off[0]) for (uint32_t i = 0; i < cregNum; ++i) {
		uint32_t b = off[0] + i * 0x1C; if (b + 0x1C > size) break; swap_RegKeyTable(out + b);
	}
	if (off[1]) for (uint32_t i = 0; i < kregNum; ++i) {
		uint32_t b = off[1] + i * 0x1C; if (b + 0x1C > size) break; swap_RegKeyTable(out + b);
	}
	uint32_t bounds[15]; int nb = 0;
	for (int i = 0; i < 14; ++i) if (off[i]) bounds[nb++] = off[i];
	bounds[nb++] = size; sort_bounds(bounds, nb);
	if (off[2]) swap_run(out, off[2], region_end(bounds, nb, off[2], size), 2);  // CReg matID u16
	if (off[3]) swap_run(out, off[3], region_end(bounds, nb, off[3], size), 2);  // KReg matID u16
	detail::swap_ResNTAB_block(out, be, off[4], size);
	detail::swap_ResNTAB_block(out, be, off[5], size);
	for (int i = 6; i < 14; ++i)                    // C/K R/G/B/A value arrays (s16)
		if (off[i]) swap_run(out, off[i], region_end(bounds, nb, off[i], size), 2);
}

// Shared helper for the simple "header counts + offset table + homogeneous value
// arrays" full/key formats. `count_words` u16 header fields starting at +0x0A are
// swapped; the offsets listed in `arrs` (block-relative reads from the BE header)
// are each swapped, then their target arrays are swapped as region-bounded runs of
// the given width (width 1 = u8 array, swapped as boundary-only). A negative
// `name_off_hdr` (>=0) names a ResNTAB header offset.
struct ArrSpec { uint32_t hdr_off; uint32_t width; };  // width: 1=u8(skip),2,4; 0=ResNTAB
static void swap_simple(uint8_t* out, const uint8_t* be, uint32_t size,
                        uint32_t hdr16_start, int hdr16_count,
                        const uint32_t* hdr32_offs, int hdr32_count,
                        const ArrSpec* arrs, int narr) {
	for (int i = 0; i < hdr16_count; ++i) sw16(out + hdr16_start + i * 2);
	for (int i = 0; i < hdr32_count; ++i) sw32(out + hdr32_offs[i]);
	uint32_t aoff[12]; int na = narr;
	uint32_t bounds[13]; int nb = 0;
	for (int i = 0; i < na; ++i) {
		aoff[i] = be32(be + arrs[i].hdr_off);
		if (aoff[i]) bounds[nb++] = aoff[i];
	}
	bounds[nb++] = size; sort_bounds(bounds, nb);
	for (int i = 0; i < na; ++i) {
		if (!aoff[i]) continue;
		if (arrs[i].width == 0) { detail::swap_ResNTAB_block(out, be, aoff[i], size); continue; }
		if (arrs[i].width == 1) continue;   // u8 array, no swap
		swap_run(out, aoff[i], region_end(bounds, nb, aoff[i], size), arrs[i].width);
	}
}

// PAK1 / J3DAnmColorKeyData (.bpk color KEY) — readAnmColor key (line 332, size 0x34):
//   +0x0C s16 mFrameMax; +0x0E..0x16 u16 x5; offsets 0x18 table(J3DAnmColorKeyTable,
//   all u16), 0x1C matID u16, 0x20 ResNTAB, 0x24..0x30 R/G/B/A values s16.
static void swap_PAK1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x34) return;
	static const uint32_t h32[] = { 0x18, 0x1C, 0x20, 0x24, 0x28, 0x2C, 0x30 };
	static const ArrSpec a[] = { {0x18,2},{0x1C,2},{0x20,0},{0x24,2},{0x28,2},{0x2C,2},{0x30,2} };
	sw16(out + 0x0C);                               // mFrameMax (s16 @0x0C)
	swap_simple(out, be, size, 0x0E, 5, h32, 7, a, 7);
}

// CLK1 / J3DAnmClusterKeyData (.blk cluster KEY) — readAnmCluster key (line 355, 0x18):
//   +0x0A s16 mFrameMax; +0x0C s32 field; 0x10 table(J3DAnmClusterKeyTable, u16),
//   0x14 weight f32.
static void swap_CLK1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x18) return;
	static const uint32_t h32[] = { 0x0C, 0x10, 0x14 };
	static const ArrSpec a[] = { {0x10,2},{0x14,4} };
	swap_simple(out, be, size, 0x0A, 1, h32, 3, a, 2);
}

// ANF1 / J3DAnmTransformFullData (.bca transform FULL) — readAnmTransform full
// (line 116, 0x24): +0x0A s16 mFrameMax; +0x0C u16; 0x14 table(J3DAnmTransformFull-
// Table, u16), 0x18 scale f32, 0x1C rot s16, 0x20 trans f32.
static void swap_ANF1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x24) return;
	static const uint32_t h32[] = { 0x14, 0x18, 0x1C, 0x20 };
	static const ArrSpec a[] = { {0x14,2},{0x18,4},{0x1C,2},{0x20,4} };
	sw16(out + 0x0A); sw16(out + 0x0C);             // mFrameMax(s16), field_0xc(u16)
	swap_simple(out, be, size, 0x0A, 0, h32, 4, a, 4);
}

// PAF1 / J3DAnmColorFullData (.bpa color FULL) — readAnmColor full (line 133, 0x34):
//   +0x0C s16 mFrameMax; +0x0E u16; 0x18 table(J3DAnmColorFullTable, u16), 0x1C matID
//   u16, 0x20 ResNTAB, 0x24..0x30 R/G/B/A values u8 (FULL uses u8, NOT s16).
static void swap_PAF1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x34) return;
	static const uint32_t h32[] = { 0x18, 0x1C, 0x20, 0x24, 0x28, 0x2C, 0x30 };
	static const ArrSpec a[] = { {0x18,2},{0x1C,2},{0x20,0},{0x24,1},{0x28,1},{0x2C,1},{0x30,1} };
	sw16(out + 0x0C); sw16(out + 0x0E);             // mFrameMax(s16), mUpdateMaterialNum
	swap_simple(out, be, size, 0x0C, 0, h32, 7, a, 7);
}

// VAF1 / J3DAnmVisibilityFullData (.bva visibility FULL) — readAnmVisibility
// (line 170, 0x18): +0x0A s16 mFrameMax; +0x0C/0x0E u16; 0x10 table
// (J3DAnmVisibilityFullTable, u16), 0x14 visibility u8 (NO swap).
static void swap_VAF1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x18) return;
	static const uint32_t h32[] = { 0x10, 0x14 };
	static const ArrSpec a[] = { {0x10,2},{0x14,1} };
	sw16(out + 0x0A); sw16(out + 0x0C); sw16(out + 0x0E);
	swap_simple(out, be, size, 0x0A, 0, h32, 2, a, 2);
}

// CLF1 / J3DAnmClusterFullData (.bla cluster FULL) — readAnmCluster full (line 184,
// 0x18): +0x0A s16 mFrameMax; +0x0C s32; 0x10 table(J3DAnmClusterFullTable, u16),
// 0x14 weight f32.
static void swap_CLF1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x18) return;
	static const uint32_t h32[] = { 0x0C, 0x10, 0x14 };
	static const ArrSpec a[] = { {0x10,2},{0x14,4} };
	swap_simple(out, be, size, 0x0A, 1, h32, 3, a, 2);
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
		case 0x54544B31: /* TTK1 */ swap_TTK1(obo, bbo, bsz); break;
		case 0x54524B31: /* TRK1 */ swap_TRK1(obo, bbo, bsz); break;
		case 0x50414B31: /* PAK1 */ swap_PAK1(obo, bbo, bsz); break;
		case 0x434C4B31: /* CLK1 */ swap_CLK1(obo, bbo, bsz); break;
		case 0x414E4631: /* ANF1 */ swap_ANF1(obo, bbo, bsz); break;
		case 0x50414631: /* PAF1 */ swap_PAF1(obo, bbo, bsz); break;
		case 0x56414631: /* VAF1 */ swap_VAF1(obo, bbo, bsz); break;
		case 0x434C4631: /* CLF1 */ swap_CLF1(obo, bbo, bsz); break;
		default: covered = false; break;
		}
		if (covered) r.blocks_covered++;
		else if (r.first_uncovered_tag == 0) r.first_uncovered_tag = tag;
		off += bsz;
	}

	r.ok = true;
	r.all_covered = (r.blocks_covered == block_num);
	return r;
}

}  // namespace smsport::assets
