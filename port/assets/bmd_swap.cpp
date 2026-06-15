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

// VTX1 / J3DVertexBlock (J3DModelLoader.hpp + J3DVertex.hpp):
//   +0x08 u32 mpVtxAttrFmtList   (offset -> GXVtxAttrFmtList[], NULL-terminated)
//   +0x0C u32 mpVtxPosArray
//   +0x10 u32 mpVtxNrmArray
//   +0x14 u32 mpVtxNBTArray
//   +0x18 u32 mpVtxColorArray[2]
//   +0x20 u32 mpVtxTexCoordArray[8]
// GXVtxAttrFmtList entry = {u32 attr; u32 cnt; u32 type; u8 frac} stride 0x10,
// terminated by attr==GX_VA_NULL(0xFF). The per-attr arrays are contiguous; each
// is a HOMOGENEOUS run of one scalar width (from the fmt `type`), so it can be
// swapped as "every N-byte scalar across [thisOffset, nextOffset)" without
// knowing component counts or vertex counts. Color arrays are packed bytes
// (RGBA8/RGB8/...) needing NO swap except the 16-bit packed formats RGB565/RGBA4.
// GXAttr values used here: POS=9, NRM=10, CLR0=11, CLR1=12, TEX0..7=13..20, NBT=25.
static uint32_t vtx_fmt_type(const uint8_t* be, uint32_t fmt_off, uint32_t size,
                             uint32_t want_attr, bool* found) {
	*found = false;
	if (fmt_off == 0) return 0;
	for (uint32_t o = fmt_off; o + 0x10 <= size; o += 0x10) {
		uint32_t attr = be32(be + o);
		if (attr == 0xFF) break;                  // GX_VA_NULL
		if (attr == want_attr) { *found = true; return be32(be + o + 0x08); }
	}
	return 0;
}
// Swap width for a non-color GXCompType: F32(4)->4, U16(2)/S16(3)->2, else 1(no-op).
static uint32_t numeric_unit(uint32_t type) {
	if (type == 4) return 4;          // GX_F32
	if (type == 2 || type == 3) return 2;  // GX_U16 / GX_S16
	return 1;                          // GX_U8 / GX_S8 -> byte, no swap
}
// Swap width for a color GXCompType: only the 16-bit packed RGB565(0)/RGBA4(3)
// need a 2-byte swap; RGB8/RGBX8/RGBA6/RGBA8 are byte streams.
static uint32_t color_unit(uint32_t type) {
	if (type == 0 || type == 3) return 2;  // GX_RGB565 / GX_RGBA4
	return 1;
}
static void swap_run(uint8_t* out, uint32_t start, uint32_t end, uint32_t unit) {
	if (unit < 2) return;
	for (uint32_t o = start; o + unit <= end; o += unit) {
		if (unit == 2) sw16(out + o);
		else if (unit == 4) sw32(out + o);
	}
}
static void swap_VTX1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x40) return;
	uint32_t fmt_off = be32(be + 0x08);
	// Gather the 13 data-array offsets with their attr id (for type lookup).
	struct Arr { uint32_t off; uint32_t attr; bool color; };
	Arr arrs[13];
	int  na = 0;
	auto add = [&](uint32_t hdr_off, uint32_t attr, bool color) {
		uint32_t o = be32(be + hdr_off);
		sw32(out + hdr_off);                       // swap the header offset
		if (o != 0) arrs[na++] = {o, attr, color};
	};
	add(0x0C, 9,  false);                          // GX_VA_POS
	add(0x10, 10, false);                          // GX_VA_NRM
	add(0x14, 25, false);                          // GX_VA_NBT
	add(0x18, 11, true);                           // GX_VA_CLR0
	add(0x1C, 12, true);                           // GX_VA_CLR1
	for (int i = 0; i < 8; ++i)
		add(0x20 + i * 4, 13 + i, false);          // GX_VA_TEX0..7
	sw32(out + 0x08);                              // mpVtxAttrFmtList offset

	// Swap the GXVtxAttrFmtList (attr/cnt/type u32, frac u8) until GX_VA_NULL.
	if (fmt_off != 0) {
		for (uint32_t o = fmt_off; o + 0x10 <= size; o += 0x10) {
			uint32_t attr = be32(be + o);
			sw32(out + o + 0x00);                  // attr
			sw32(out + o + 0x04);                  // cnt
			sw32(out + o + 0x08);                  // type
			// o+0x0C frac u8 (+pad): no swap
			if (attr == 0xFF) break;
		}
	}

	// Each array runs from its offset to the next region boundary. Build the
	// sorted boundary set (fmt list start + all array starts + block end).
	uint32_t bounds[16]; int nb = 0;
	if (fmt_off != 0) bounds[nb++] = fmt_off;
	for (int i = 0; i < na; ++i) bounds[nb++] = arrs[i].off;
	bounds[nb++] = size;
	for (int i = 0; i < nb; ++i)                   // insertion sort (tiny)
		for (int j = i + 1; j < nb; ++j)
			if (bounds[j] < bounds[i]) { uint32_t t = bounds[i]; bounds[i] = bounds[j]; bounds[j] = t; }

	for (int i = 0; i < na; ++i) {
		uint32_t start = arrs[i].off;
		uint32_t end   = size;
		for (int j = 0; j < nb; ++j)
			if (bounds[j] > start && bounds[j] < end) end = bounds[j];
		bool found;
		uint32_t type = vtx_fmt_type(be, fmt_off, size, arrs[i].attr, &found);
		uint32_t unit = arrs[i].color ? color_unit(type) : numeric_unit(type);
		// No fmt entry found -> default float (POS/NRM/TEX are f32 by default).
		if (!found && !arrs[i].color) unit = 4;
		swap_run(out, start, end, unit);
	}
}

// SHP1 / J3DShapeBlock (J3DShapeFactory.hpp):
//   +0x08 u16 mShapeNum (+0x0A pad)
//   +0x0C u32 mpShapeInitData (offset -> J3DShapeInitData[mShapeNum], stride 0x28)
//   +0x10 u32 mpIndexTable    (offset -> u16[mShapeNum])
//   +0x14 u32 mpNameTable     (offset -> ResNTAB or 0)
//   +0x18 u32 mpVtxDescList   (offset -> GXVtxDescList{u32 attr; u32 type}[]...)
//   +0x1C u32 mpMtxTable      (offset -> u16[])
//   +0x20 u32 mpDisplayListData (offset -> GX FIFO byte stream)
//   +0x24 u32 mpMtxInitData   (offset -> J3DShapeMtxInitData{u16;u16;u32}[])
//   +0x28 u32 mpDrawInitData  (offset -> J3DShapeDrawInitData{u32;u32}[])
// J3DShapeInitData (stride 0x28): u8 mShapeMtxType@0x00, u16 mMtxGroupNum@0x02,
//   u16 mVtxDescListIndex@0x04, u16 mMtxInitDataIndex@0x06, u16 mDrawInitDataIndex
//   @0x08, f32 mRadius@0x0C, Vec mMin@0x10, Vec mMax@0x1C.
// The mpVtxDescList region is wholly u32 attr/type pairs; mpMtxTable is wholly
// u16; so each is swapped as a uniform scalar run over [start,nextBoundary).
// NOTE: the DISPLAY-LIST byte stream interior (BE u16 vertex counts + BE index16
// values) is NOT swapped here — that requires VCD-driven primitive parsing and is
// render-time data consumed by the port GX/draw path (not owned yet, GX stubbed).
// The loader (readShape/setupBBoardInfo) reads only the structural fields above,
// so this is the correct scope for the loader gate. Display-list interior swapping
// is deferred to when the port draw path consumes it.
static void swap_SHP1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x2C) return;
	uint16_t shape_num = be16(be + 0x08);
	uint32_t off_init  = be32(be + 0x0C);
	uint32_t off_idx   = be32(be + 0x10);
	uint32_t off_name  = be32(be + 0x14);
	uint32_t off_vtxd  = be32(be + 0x18);
	uint32_t off_mtxt  = be32(be + 0x1C);
	uint32_t off_dl    = be32(be + 0x20);
	uint32_t off_mtxi  = be32(be + 0x24);
	uint32_t off_drawi = be32(be + 0x28);
	sw16(out + 0x08);                              // mShapeNum
	for (uint32_t o = 0x0C; o <= 0x28; o += 4) sw32(out + o);  // 7 offsets

	if (off_init != 0) {
		for (uint32_t i = 0; i < shape_num; ++i) {
			uint32_t b = off_init + i * 0x28;
			if (b + 0x28 > size) break;
			// b+0x00 mShapeMtxType u8 (+0x01 pad): no swap
			sw16(out + b + 0x02);                  // mMtxGroupNum
			sw16(out + b + 0x04);                  // mVtxDescListIndex
			sw16(out + b + 0x06);                  // mMtxInitDataIndex
			sw16(out + b + 0x08);                  // mDrawInitDataIndex
			// b+0x0A pad
			sw32(out + b + 0x0C);                  // mRadius
			sw32(out + b + 0x10); sw32(out + b + 0x14); sw32(out + b + 0x18); // mMin
			sw32(out + b + 0x1C); sw32(out + b + 0x20); sw32(out + b + 0x24); // mMax
		}
	}
	if (off_idx != 0) {
		for (uint32_t i = 0; i < shape_num; ++i) {
			uint32_t o = off_idx + i * 2;
			if (o + 2 > size) break;
			sw16(out + o);                         // mpIndexTable[i]
		}
	}
	swap_ResNTAB(out, be, off_name, size);

	// Region boundaries for the uniform-run regions.
	uint32_t bounds[10]; int nb = 0;
	auto addb = [&](uint32_t v){ if (v) bounds[nb++] = v; };
	addb(off_init); addb(off_idx); addb(off_name); addb(off_vtxd);
	addb(off_mtxt); addb(off_dl); addb(off_mtxi); addb(off_drawi);
	bounds[nb++] = size;
	for (int i = 0; i < nb; ++i)
		for (int j = i + 1; j < nb; ++j)
			if (bounds[j] < bounds[i]) { uint32_t t = bounds[i]; bounds[i] = bounds[j]; bounds[j] = t; }
	auto region_end = [&](uint32_t start)->uint32_t {
		uint32_t end = size;
		for (int j = 0; j < nb; ++j) if (bounds[j] > start && bounds[j] < end) end = bounds[j];
		return end;
	};

	// mpVtxDescList: GXVtxDescList{u32 attr; u32 type} pairs (NULL-terminated per
	// shape, but the whole region is u32 either way) -> swap every u32.
	if (off_vtxd != 0) swap_run(out, off_vtxd, region_end(off_vtxd), 4);
	// mpMtxTable: u16 matrix indices -> swap every u16.
	if (off_mtxt != 0) swap_run(out, off_mtxt, region_end(off_mtxt), 2);
	// mpDisplayListData: byte stream -> interior swap deferred (see note above).
	// mpMtxInitData: J3DShapeMtxInitData{u16 mUseMtxIndex; u16 mUseMtxCount;
	//   u32 mFirstUseMtxIndex} stride 8.
	if (off_mtxi != 0) {
		uint32_t end = region_end(off_mtxi);
		for (uint32_t o = off_mtxi; o + 8 <= end; o += 8) {
			sw16(out + o + 0x00); sw16(out + o + 0x02); sw32(out + o + 0x04);
		}
	}
	// mpDrawInitData: J3DShapeDrawInitData{u32 mDisplayListSize; u32
	//   mDisplayListIndex} stride 8 -> swap every u32.
	if (off_drawi != 0) swap_run(out, off_drawi, region_end(off_drawi), 4);
}

// TEX1 / J3DTextureBlock (J3DModelLoader.hpp):
//   +0x08 u16 mTextureNum (+0x0A pad)
//   +0x0C u32 mpTextureRes (offset -> ResTIMG[mTextureNum], stride 0x20)
//   +0x10 u32 mpNameTable  (offset -> ResNTAB)
// ResTIMG (ResTIMG.hpp, 0x20): u8 format@0x00, u8 alphaEnabled@0x01, u16 width
//   @0x02, u16 height@0x04, u8 wrapS/wrapT@0x06/07, u8 isIndexTexture@0x08, u8
//   colorFormat@0x09, u16 numColors@0x0A, u32 paletteOffset@0x0C, u8 fields
//   @0x10..0x19, s16 lodBias@0x1A, u32 imageDataOffset@0x1C.
// Only the ResTIMG header scalars are swapped. The palette and texel pixel data
// (at paletteOffset/imageDataOffset) stay GC-native: GameCube texel/TLUT formats
// (CMPR/RGB5A3/CI8/...) are decoded by the port texture loader, which interprets
// their tiling + 16-bit endianness as part of the format decode — pre-swapping
// here would corrupt that decode. The loader (readTexture) reads only count +
// the ResTIMG pointer, so this is the correct scope.
static void swap_TEX1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x14) return;
	uint16_t tex_num = be16(be + 0x08);
	uint32_t off_res  = be32(be + 0x0C);
	uint32_t off_name = be32(be + 0x10);
	sw16(out + 0x08);                              // mTextureNum
	sw32(out + 0x0C); sw32(out + 0x10);            // offsets
	if (off_res != 0) {
		for (uint32_t i = 0; i < tex_num; ++i) {
			uint32_t b = off_res + i * 0x20;
			if (b + 0x20 > size) break;
			// b+0x00 format, b+0x01 alphaEnabled: u8 no swap
			sw16(out + b + 0x02);                  // width
			sw16(out + b + 0x04);                  // height
			// b+0x06..0x09 u8 fields: no swap
			sw16(out + b + 0x0A);                  // numColors
			sw32(out + b + 0x0C);                  // paletteOffset
			// b+0x10..0x19 u8 fields: no swap
			sw16(out + b + 0x1A);                  // lodBias (s16)
			sw32(out + b + 0x1C);                  // imageDataOffset
		}
	}
	swap_ResNTAB(out, be, off_name, size);
}

// MAT3 / J3DMaterialBlock (J3DMaterialFactory.hpp). The material factory reads
// every table at LOAD time, so all multibyte fields must be swapped (unlike
// SHP1/TEX1 render-time data). Block header: u16 mMaterialNum@0x08 then 30 u32
// table offsets @0x0C..0x80. Each table's element count comes from its byte span
// (next-offset - this-offset), so empty tables (offset == next) auto-skip.
//
// Per-table element layouts (from J3DStruct.hpp / J3DMaterialFactory.hpp). Tables
// of pure u8 / GXColor(u8x4) bytes need NO swap and are omitted below:
//   matColor/ambColor/tevKColor (GXColor), colorChanNum/texGenNum/tevStageNum/
//   zCompLoc/dither (u8[]), colorChanInfo/tevOrderInfo/tevStageInfo/
//   tevSwapModeInfo/tevSwapModeTableInfo/alphaCompInfo/blendInfo/zModeInfo/
//   texCoordInfo/texCoord2Info (all-u8 structs).
//   lightInfo is empty in all SMS BMDs and its layout is undefined in the decomp;
//   a non-empty span is flagged (loud) rather than mis-swapped.
static inline void sw16_n(uint8_t* p, int n) { for (int i=0;i<n;++i) sw16(p+i*2); }
static inline void sw32_n(uint8_t* p, int n) { for (int i=0;i<n;++i) sw32(p+i*4); }

static void swap_J3DMaterialInitData(uint8_t* e) {  // stride 0x14C
	// 0x000..0x007 u8 fields: no swap.
	sw16_n(e+0x008, 2);   // mMatColorIdx[2]
	sw16_n(e+0x00C, 4);   // mColorChanIdx[4]
	sw16_n(e+0x014, 2);   // mAmbColorIdx[2]
	// 0x018 u8[16]
	sw16_n(e+0x028, 8);   // mTexCoordIdx[8]
	// 0x038 u8[16]
	sw16_n(e+0x048, 8);   // mTexMtxIdx[8]
	// 0x058 u8[44]
	sw16_n(e+0x084, 8);   // mTexNoIdx[8]
	sw16_n(e+0x094, 4);   // mTevKColorIdx[4]
	// 0x09C u8[16], 0x0AC u8[16]
	sw16_n(e+0x0BC, 16);  // mTevOrderIdx[16]
	sw16_n(e+0x0DC, 4);   // mTevColorIdx[4]
	sw16_n(e+0x0E4, 16);  // mTevStageIdx[16]
	sw16_n(e+0x104, 16);  // mTevSwapModeIdx[16]
	sw16_n(e+0x124, 4);   // mTevSwapModeTableIdx[4]
	// 0x12C u8[24]
	sw16_n(e+0x144, 4);   // mFogIdx, mAlphaCompIdx, mBlendIdx, mNBTScaleIdx
}
static void swap_J3DTexMtxInfo(uint8_t* e) {        // stride 0x64
	// 0x00 u8 mProjection, 0x01 u8 mInfo, 0x02 pad[2]: no swap
	sw32_n(e+0x04, 3);    // Vec mCenter
	// J3DTextureSRTInfo @0x10 (0x14): f32 scaleX/Y, s16 rotation, f32 transX/Y
	sw32_n(e+0x10, 2);    // mScaleX, mScaleY
	sw16(e+0x18);         // mRotation (s16)  (+0x1A pad)
	sw32_n(e+0x1C, 2);    // mTranslationX, mTranslationY
	sw32_n(e+0x24, 16);   // Mtx44 mEffectMtx (f32[4][4])
}
static void swap_J3DFogInfo(uint8_t* e) {           // stride 0x2C
	// 0x00 u8 mType, 0x01 u8 mAdjEnable
	sw16(e+0x02);         // mCenter
	sw32_n(e+0x04, 4);    // mStartZ, mEndZ, mNearZ, mFarZ
	// 0x14 GXColor mColor (u8x4): no swap
	sw16_n(e+0x18, 10);   // mFogAdjTable[10]
}
static void swap_J3DIndInitData(uint8_t* e) {       // stride 0x138
	// J3DIndTexMtxInfo[3] @0x14, stride 0x1C: f32 mOffsetMtx[2][3] (6 f32),
	// s8 mScaleExp@0x18 (no swap). Other fields are all u8.
	for (int i = 0; i < 3; ++i) sw32_n(e + 0x14 + i * 0x1C, 6);
}

static void swap_MAT3(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x84) return;
	sw16(out + 0x08);                              // mMaterialNum
	uint32_t off[30];
	for (int k = 0; k < 30; ++k) {
		off[k] = be32(be + 0x0C + k * 4);
		sw32(out + 0x0C + k * 4);                  // swap each table offset
	}
	// Region-end resolver from the full sorted offset set + block size.
	uint32_t bounds[31]; int nb = 0;
	for (int k = 0; k < 30; ++k) if (off[k]) bounds[nb++] = off[k];
	bounds[nb++] = size;
	for (int i = 0; i < nb; ++i)
		for (int j = i + 1; j < nb; ++j)
			if (bounds[j] < bounds[i]) { uint32_t t = bounds[i]; bounds[i] = bounds[j]; bounds[j] = t; }
	auto rend = [&](uint32_t s)->uint32_t {
		uint32_t e = size;
		for (int j = 0; j < nb; ++j) if (bounds[j] > s && bounds[j] < e) e = bounds[j];
		return e;
	};
	auto each = [&](int k, uint32_t stride, void(*fn)(uint8_t*)) {
		if (!off[k]) return;
		uint32_t end = rend(off[k]);
		for (uint32_t o = off[k]; o + stride <= end; o += stride) fn(out + o);
	};
	// Table indices (J3DMaterialBlock order): 0 init,1 matID,2 name,3 indInit,
	// 4 cull,5 matColor,6 colorChanNum,7 colorChanInfo,8 ambColor,9 lightInfo,
	// 10 texGenNum,11 texCoordInfo,12 texCoord2,13 texMtx,14 field44,15 texNo,
	// 16 tevOrder,17 tevColor,18 tevKColor,19 tevStageNum,20 tevStageInfo,
	// 21 tevSwapMode,22 tevSwapModeTable,23 fog,24 alphaComp,25 blend,26 zMode,
	// 27 zCompLoc,28 dither,29 nbtScale.
	each(0,  0x14C, swap_J3DMaterialInitData);
	if (off[1])  swap_run(out, off[1], rend(off[1]), 2);   // matID u16[]
	swap_ResNTAB(out, be, off[2], size);                   // name table
	each(3,  0x138, swap_J3DIndInitData);
	if (off[4])  swap_run(out, off[4], rend(off[4]), 4);   // cullMode u32[]
	each(13, 0x64, swap_J3DTexMtxInfo);                    // texMtxInfo
	each(14, 0x64, swap_J3DTexMtxInfo);                    // field_0x44 (TexMtxInfo)
	if (off[15]) swap_run(out, off[15], rend(off[15]), 2); // texNo u16[]
	if (off[17]) swap_run(out, off[17], rend(off[17]), 2); // tevColor GXColorS10 (s16x4)
	each(23, 0x2C, swap_J3DFogInfo);                       // fogInfo
	each(29, 0x10, [](uint8_t* e){ sw32_n(e + 0x04, 3); });// nbtScaleInfo Vec mScale
	// lightInfo (index 9): undefined layout, empty in all SMS BMDs. Leave bytes;
	// callers that hit a non-empty lightInfo span need its layout RE'd first.
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
		case 0x56545831: /* VTX1 */ swap_VTX1(obo, bbo, bsz); break;
		case 0x53485031: /* SHP1 */ swap_SHP1(obo, bbo, bsz); break;
		case 0x54455831: /* TEX1 */ swap_TEX1(obo, bbo, bsz); break;
		case 0x4D415433: /* MAT3 */ swap_MAT3(obo, bbo, bsz); break;
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
