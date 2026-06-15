#include "anm_swap.h"
#include "bmd_blocks.h"  // detail::swap_ResNTAB_block (shared name-table swapper)
#include "rarc.h"        // be16/be32 explicit big-endian reads

namespace smsport::assets {

// ---- in-place big-endian -> host swaps on the OUTPUT copy --------------------
static inline void sw16(uint8_t* p) {
	uint8_t t = p[0]; p[0] = p[1]; p[1] = t;
}
static inline void sw32(uint8_t* p) {
	uint8_t t;
	t = p[0]; p[0] = p[3]; p[3] = t;
	t = p[1]; p[1] = p[2]; p[2] = t;
}
// Swap a contiguous homogeneous run [start,end) of `unit`-byte scalars (2 or 4).
static void swap_run(uint8_t* out, uint32_t start, uint32_t end, uint32_t unit) {
	if (unit < 2) return;                          // byte data: no swap
	for (uint32_t o = start; o + unit <= end; o += unit) {
		if (unit == 2) sw16(out + o);
		else if (unit == 4) sw32(out + o);
	}
}
static inline void rntab(uint8_t* out, const uint8_t* be, uint32_t off,
                         uint32_t size) {
	detail::swap_ResNTAB_block(out, be, off, size);
}

// Region-boundary resolver: every offset field in a J3D block points to the start
// of a contiguous region; the region ends at the NEXT-greater offset (or block
// end). Collect the offsets, sort, then end(start) finds the next boundary — so a
// homogeneous run can be swapped without knowing element counts (matches the
// bmd_swap MAT3/VTX1/SHP1 technique). 24 slots cover the widest anm block (TTK1).
struct Bounds {
	uint32_t v[24];
	int      n = 0;
	void add(uint32_t o) { if (o) v[n++] = o; }
	void seal(uint32_t size) {
		v[n++] = size;
		for (int i = 0; i < n; ++i)
			for (int j = i + 1; j < n; ++j)
				if (v[j] < v[i]) { uint32_t t = v[i]; v[i] = v[j]; v[j] = t; }
	}
	uint32_t end(uint32_t start, uint32_t size) const {
		uint32_t e = size;
		for (int i = 0; i < n; ++i) if (v[i] > start && v[i] < e) e = v[i];
		return e;
	}
};

// =============================================================================
// KEY family (J3DAnmKeyLoader_v15). All keyframe tables are J3DAnmKeyTableBase
// runs (3xu16) so they swap as u16; value arrays are f32 (scale/trans/weight/
// SRTCenter), s16 (rotation, color, tevreg) or u8 (updateTexMtxID — no swap).
// =============================================================================

// ANK1 / J3DAnmTransformKeyData (readAnmTransform). Header @0x08:
//   u8 mAttribute@8, u8 mDecShift@9, s16 mFrameMax@0xA, u16 field_0xc@0xC,
//   int field_0x10@0x10, s32 mTableOffset@0x14, mScaleOffset@0x18,
//   mRotOffset@0x1c, mTransOffset@0x20.
static void swap_ANK1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x24) return;
	sw16(out + 0x0A);                              // mFrameMax
	sw16(out + 0x0C);                              // field_0xc
	sw32(out + 0x10);                              // field_0x10
	uint32_t tbl = be32(be + 0x14), scl = be32(be + 0x18);
	uint32_t rot = be32(be + 0x1C), trn = be32(be + 0x20);
	sw32(out + 0x14); sw32(out + 0x18); sw32(out + 0x1C); sw32(out + 0x20);
	Bounds b; b.add(tbl); b.add(scl); b.add(rot); b.add(trn); b.seal(size);
	if (tbl) swap_run(out, tbl, b.end(tbl, size), 2);  // J3DAnmTransformKeyTable
	if (scl) swap_run(out, scl, b.end(scl, size), 4);  // scale f32
	if (rot) swap_run(out, rot, b.end(rot, size), 2);  // rotation s16
	if (trn) swap_run(out, trn, b.end(trn, size), 4);  // translate f32
}

// TTK1 / J3DAnmTextureSRTKeyData (readAnmTextureSRT). Size 0x60.
static void swap_TTK1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x60) return;
	// Header scalars (u16): field_0xa, _c, _e, _10, _12 then _34,_36,_38,_3a.
	sw16(out + 0x0A); sw16(out + 0x0C); sw16(out + 0x0E);
	sw16(out + 0x10); sw16(out + 0x12);
	sw16(out + 0x34); sw16(out + 0x36); sw16(out + 0x38); sw16(out + 0x3A);
	uint32_t tbl   = be32(be + 0x14);  // J3DAnmTransformKeyTable (u16)
	uint32_t matid = be32(be + 0x18);  // updateMaterialID u16[]
	uint32_t ntab1 = be32(be + 0x1C);  // ResNTAB
	uint32_t txmid = be32(be + 0x20);  // updateTexMtxID u8[] (no swap)
	uint32_t cen   = be32(be + 0x24);  // SRTCenter Vec f32
	uint32_t scl   = be32(be + 0x28);  // scale f32
	uint32_t rot   = be32(be + 0x2C);  // rotation s16
	uint32_t trn   = be32(be + 0x30);  // translate f32
	uint32_t tbl2  = be32(be + 0x3C);  // post J3DAnmTransformKeyTable (u16)
	uint32_t pmid  = be32(be + 0x40);  // post updateMaterialID u16[]
	uint32_t ntab2 = be32(be + 0x44);  // ResNTAB (post)
	uint32_t ptxm  = be32(be + 0x48);  // post updateTexMtxID u8[] (no swap)
	uint32_t pcen  = be32(be + 0x4C);  // post SRTCenter Vec f32
	uint32_t pscl  = be32(be + 0x50);  // post scale f32
	uint32_t prot  = be32(be + 0x54);  // post rotation s16
	uint32_t ptrn  = be32(be + 0x58);  // post translate f32
	// Swap every declared s32 offset field in the header (incl. unread field_0x5c).
	for (uint32_t o = 0x14; o <= 0x30; o += 4) sw32(out + o);
	for (uint32_t o = 0x3C; o <= 0x5C; o += 4) sw32(out + o);
	Bounds b;
	b.add(tbl); b.add(matid); b.add(ntab1); b.add(txmid); b.add(cen);
	b.add(scl); b.add(rot); b.add(trn); b.add(tbl2); b.add(pmid);
	b.add(ntab2); b.add(ptxm); b.add(pcen); b.add(pscl); b.add(prot); b.add(ptrn);
	b.seal(size);
	if (tbl)   swap_run(out, tbl,   b.end(tbl, size),   2);
	if (matid) swap_run(out, matid, b.end(matid, size), 2);
	rntab(out, be, ntab1, size);
	// txmid: u8[] no swap
	if (cen)   swap_run(out, cen,   b.end(cen, size),   4);
	if (scl)   swap_run(out, scl,   b.end(scl, size),   4);
	if (rot)   swap_run(out, rot,   b.end(rot, size),   2);
	if (trn)   swap_run(out, trn,   b.end(trn, size),   4);
	if (tbl2)  swap_run(out, tbl2,  b.end(tbl2, size),  2);
	if (pmid)  swap_run(out, pmid,  b.end(pmid, size),  2);
	rntab(out, be, ntab2, size);
	// ptxm: u8[] no swap
	if (pcen)  swap_run(out, pcen,  b.end(pcen, size),  4);
	if (pscl)  swap_run(out, pscl,  b.end(pscl, size),  4);
	if (prot)  swap_run(out, prot,  b.end(prot, size),  2);
	if (ptrn)  swap_run(out, ptrn,  b.end(ptrn, size),  4);
}

// PAK1 / J3DAnmColorKeyData (readAnmColor key). Color values are s16 (KEY family).
static void swap_PAK1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x34) return;
	sw16(out + 0x0C); sw16(out + 0x0E);            // mFrameMax, mUpdateMaterialNum
	sw16(out + 0x10); sw16(out + 0x12); sw16(out + 0x14); sw16(out + 0x16);
	uint32_t tbl = be32(be + 0x18), mid = be32(be + 0x1C), ntb = be32(be + 0x20);
	uint32_t r = be32(be + 0x24), g = be32(be + 0x28),
	         bl = be32(be + 0x2C), a = be32(be + 0x30);
	for (uint32_t o = 0x18; o <= 0x30; o += 4) sw32(out + o);
	Bounds bnd; bnd.add(tbl); bnd.add(mid); bnd.add(ntb);
	bnd.add(r); bnd.add(g); bnd.add(bl); bnd.add(a); bnd.seal(size);
	if (tbl) swap_run(out, tbl, bnd.end(tbl, size), 2);  // J3DAnmColorKeyTable u16
	if (mid) swap_run(out, mid, bnd.end(mid, size), 2);  // updateMaterialID u16[]
	rntab(out, be, ntb, size);
	if (r)  swap_run(out, r,  bnd.end(r, size),  2);     // R/G/B/A s16
	if (g)  swap_run(out, g,  bnd.end(g, size),  2);
	if (bl) swap_run(out, bl, bnd.end(bl, size), 2);
	if (a)  swap_run(out, a,  bnd.end(a, size),  2);
}

// CLK1 / J3DAnmClusterKeyData (readAnmCluster key).
static void swap_CLK1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x18) return;
	sw16(out + 0x0A);                              // mFrameMax
	sw32(out + 0x0C);                              // field_0xc (unread)
	uint32_t tbl = be32(be + 0x10), wt = be32(be + 0x14);
	sw32(out + 0x10); sw32(out + 0x14);
	Bounds b; b.add(tbl); b.add(wt); b.seal(size);
	if (tbl) swap_run(out, tbl, b.end(tbl, size), 2);  // J3DAnmClusterKeyTable u16
	if (wt)  swap_run(out, wt,  b.end(wt, size),  4);  // weight f32
}

// TRK1 / J3DAnmTevRegKeyData (readAnmTevReg). The C/K register tables are
// J3DAnmCRegKeyTable/J3DAnmKRegKeyTable: 4x J3DAnmKeyTableBase (12 u16) + a u8
// colorId + 3 pad, stride 0x1C — swap the 12 u16, leave the trailing bytes.
static void swap_RegKeyTable(uint8_t* out, uint32_t start, uint32_t end) {
	for (uint32_t o = start; o + 0x1C <= end; o += 0x1C)
		for (int k = 0; k < 12; ++k) sw16(out + o + k * 2);   // 4x KeyTableBase
}
static void swap_TRK1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x58) return;
	for (uint32_t o = 0x0A; o <= 0x1E; o += 2) sw16(out + o);  // mFrameMax + 10 u16
	uint32_t ct = be32(be + 0x20), kt = be32(be + 0x24);
	uint32_t cmid = be32(be + 0x28), kmid = be32(be + 0x2C);
	uint32_t cnt = be32(be + 0x30), knt = be32(be + 0x34);
	uint32_t cr = be32(be + 0x38), cg = be32(be + 0x3C),
	         cb = be32(be + 0x40), ca = be32(be + 0x44);
	uint32_t kr = be32(be + 0x48), kg = be32(be + 0x4C),
	         kb = be32(be + 0x50), ka = be32(be + 0x54);
	for (uint32_t o = 0x20; o <= 0x54; o += 4) sw32(out + o);
	Bounds b;
	b.add(ct); b.add(kt); b.add(cmid); b.add(kmid); b.add(cnt); b.add(knt);
	b.add(cr); b.add(cg); b.add(cb); b.add(ca);
	b.add(kr); b.add(kg); b.add(kb); b.add(ka); b.seal(size);
	if (ct) swap_RegKeyTable(out, ct, b.end(ct, size));
	if (kt) swap_RegKeyTable(out, kt, b.end(kt, size));
	if (cmid) swap_run(out, cmid, b.end(cmid, size), 2);
	if (kmid) swap_run(out, kmid, b.end(kmid, size), 2);
	rntab(out, be, cnt, size);
	rntab(out, be, knt, size);
	if (cr) swap_run(out, cr, b.end(cr, size), 2);  // C reg R/G/B/A s16
	if (cg) swap_run(out, cg, b.end(cg, size), 2);
	if (cb) swap_run(out, cb, b.end(cb, size), 2);
	if (ca) swap_run(out, ca, b.end(ca, size), 2);
	if (kr) swap_run(out, kr, b.end(kr, size), 2);  // K reg R/G/B/A s16
	if (kg) swap_run(out, kg, b.end(kg, size), 2);
	if (kb) swap_run(out, kb, b.end(kb, size), 2);
	if (ka) swap_run(out, ka, b.end(ka, size), 2);
}

// J3DAnmVtxColorIndexData on disk: u16 mNum@0, (pad@2), u32 mpData@4 — stride 8.
// (Shared by VCK1 + VCF1.) `count` = mAnmTableNum[i].
static void swap_VtxColorIndexData(uint8_t* out, uint32_t start, uint32_t count,
                                   uint32_t size) {
	for (uint32_t i = 0; i < count; ++i) {
		uint32_t o = start + i * 8;
		if (o + 8 > size) break;
		sw16(out + o + 0);   // mNum
		sw32(out + o + 4);   // mpData (file offset; *2 relocation is host-side)
	}
}

// VCK1 / J3DAnmVtxColorKeyData (readAnmVtxColor key). Color values are s16.
// NOTE: not present in observed SMS scenes; implemented for completeness. The
// port loader's J3DAnmVtxColorIndexData overlay has a latent LP64 stride issue
// (host struct is 16 bytes vs the 8-byte file element) independent of this swap.
static void swap_VCK1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x40) return;
	sw16(out + 0x0A);                              // mFrameMax
	sw16(out + 0x0C); sw16(out + 0x0E);            // mAnmTableNum[2]
	sw32(out + 0x10); sw32(out + 0x14);            // mIndexNum[2] (u32 in KEY)
	uint16_t num0 = be16(be + 0x0C), num1 = be16(be + 0x0E);
	uint32_t t0 = be32(be + 0x18), t1 = be32(be + 0x1C);
	uint32_t d0 = be32(be + 0x20), d1 = be32(be + 0x24);
	uint32_t p0 = be32(be + 0x28), p1 = be32(be + 0x2C);
	uint32_t r = be32(be + 0x30), g = be32(be + 0x34),
	         bl = be32(be + 0x38), a = be32(be + 0x3C);
	for (uint32_t o = 0x18; o <= 0x3C; o += 4) sw32(out + o);
	Bounds b;
	b.add(t0); b.add(t1); b.add(d0); b.add(d1); b.add(p0); b.add(p1);
	b.add(r); b.add(g); b.add(bl); b.add(a); b.seal(size);
	if (t0) swap_run(out, t0, b.end(t0, size), 2);  // J3DAnmColorKeyTable u16
	if (t1) swap_run(out, t1, b.end(t1, size), 2);
	if (d0) swap_VtxColorIndexData(out, d0, num0, size);
	if (d1) swap_VtxColorIndexData(out, d1, num1, size);
	if (p0) swap_run(out, p0, b.end(p0, size), 2);  // index pointer u16[]
	if (p1) swap_run(out, p1, b.end(p1, size), 2);
	if (r)  swap_run(out, r,  b.end(r, size),  2);  // R/G/B/A s16
	if (g)  swap_run(out, g,  b.end(g, size),  2);
	if (bl) swap_run(out, bl, b.end(bl, size), 2);
	if (a)  swap_run(out, a,  b.end(a, size),  2);
}

// =============================================================================
// FULL family (J3DAnmFullLoader_v15). FULL color values are u8 (no swap); tables
// are plain u16 runs except J3DAnmTexPatternFullTable (mixed, stride 8).
// =============================================================================

// ANF1 / J3DAnmTransformFullData (readAnmTransform full).
static void swap_ANF1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x24) return;
	sw16(out + 0x0A); sw16(out + 0x0C);            // mFrameMax, field_0xc
	uint32_t tbl = be32(be + 0x14), scl = be32(be + 0x18);
	uint32_t rot = be32(be + 0x1C), trn = be32(be + 0x20);
	for (uint32_t o = 0x14; o <= 0x20; o += 4) sw32(out + o);
	Bounds b; b.add(tbl); b.add(scl); b.add(rot); b.add(trn); b.seal(size);
	if (tbl) swap_run(out, tbl, b.end(tbl, size), 2);  // J3DAnmTransformFullTable
	if (scl) swap_run(out, scl, b.end(scl, size), 4);  // scale f32
	if (rot) swap_run(out, rot, b.end(rot, size), 2);  // rotation s16
	if (trn) swap_run(out, trn, b.end(trn, size), 4);  // translate f32
}

// PAF1 / J3DAnmColorFullData (readAnmColor full). Color values are u8 (no swap).
static void swap_PAF1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x34) return;
	sw16(out + 0x0C); sw16(out + 0x0E);            // mFrameMax, mUpdateMaterialNum
	uint32_t tbl = be32(be + 0x18), mid = be32(be + 0x1C), ntb = be32(be + 0x20);
	for (uint32_t o = 0x18; o <= 0x30; o += 4) sw32(out + o);
	Bounds b; b.add(tbl); b.add(mid); b.add(ntb);
	b.add(be32(be + 0x24)); b.add(be32(be + 0x28));
	b.add(be32(be + 0x2C)); b.add(be32(be + 0x30)); b.seal(size);
	if (tbl) swap_run(out, tbl, b.end(tbl, size), 2);  // J3DAnmColorFullTable u16
	if (mid) swap_run(out, mid, b.end(mid, size), 2);  // updateMaterialID u16[]
	rntab(out, be, ntb, size);
	// R/G/B/A values are u8[] in the FULL family: no swap.
}

// TPT1 / J3DAnmTexPatternFullData (readAnmTexPattern). The table is
// J3DAnmTexPatternFullTable (stride 8: u16 mMaxFrame@0, u16 mOffset@2, u8 mTexNo@4
// (+pad), u16 _6@6) — NOT a plain u16 run, so swap per element.
static void swap_TPT1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x20) return;
	sw16(out + 0x0A); sw16(out + 0x0C); sw16(out + 0x0E);  // mFrameMax,_c,_e
	uint32_t tbl = be32(be + 0x10), val = be32(be + 0x14);
	uint32_t mid = be32(be + 0x18), ntb = be32(be + 0x1C);
	for (uint32_t o = 0x10; o <= 0x1C; o += 4) sw32(out + o);
	Bounds b; b.add(tbl); b.add(val); b.add(mid); b.add(ntb); b.seal(size);
	if (tbl) {
		uint32_t end = b.end(tbl, size);
		for (uint32_t o = tbl; o + 8 <= end; o += 8) {
			sw16(out + o + 0);   // mMaxFrame
			sw16(out + o + 2);   // mOffset
			// o+4 u8 mTexNo (+o+5 pad): no swap
			sw16(out + o + 6);   // _6
		}
	}
	if (val) swap_run(out, val, b.end(val, size), 2);  // mTextureIndex u16[]
	if (mid) swap_run(out, mid, b.end(mid, size), 2);  // updateMaterialID u16[]
	rntab(out, be, ntb, size);
}

// CLF1 / J3DAnmClusterFullData (readAnmCluster full).
static void swap_CLF1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x18) return;
	sw16(out + 0x0A);                              // mFrameMax
	sw32(out + 0x0C);                              // field_0xc (unread)
	uint32_t tbl = be32(be + 0x10), wt = be32(be + 0x14);
	sw32(out + 0x10); sw32(out + 0x14);
	Bounds b; b.add(tbl); b.add(wt); b.seal(size);
	if (tbl) swap_run(out, tbl, b.end(tbl, size), 2);  // J3DAnmClusterFullTable u16
	if (wt)  swap_run(out, wt,  b.end(wt, size),  4);  // weight f32
}

// VAF1 / J3DAnmVisibilityFullData (readAnmVisibility). Values are u8 (no swap).
static void swap_VAF1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x18) return;
	sw16(out + 0x0A); sw16(out + 0x0C); sw16(out + 0x0E);  // mFrameMax,_c,_e
	uint32_t tbl = be32(be + 0x10);
	sw32(out + 0x10); sw32(out + 0x14);
	Bounds b; b.add(tbl); b.add(be32(be + 0x14)); b.seal(size);
	if (tbl) swap_run(out, tbl, b.end(tbl, size), 2);  // J3DAnmVisibilityFullTable
	// mVisibility values are u8[]: no swap.
}

// VCF1 / J3DAnmVtxColorFullData (readAnmVtxColor full). Color values are u8.
// Same not-observed/LP64 caveat as VCK1.
static void swap_VCF1(uint8_t* out, const uint8_t* be, uint32_t size) {
	if (size < 0x40) return;
	sw16(out + 0x0A);                              // mFrameMax
	sw16(out + 0x0C); sw16(out + 0x0E);            // mAnmTableNum[2]
	sw16(out + 0x10); sw16(out + 0x12);            // mIndexNum[2] (u16 in FULL)
	sw32(out + 0x14);                              // field_0x14
	uint16_t num0 = be16(be + 0x0C), num1 = be16(be + 0x0E);
	uint32_t t0 = be32(be + 0x18), t1 = be32(be + 0x1C);
	uint32_t d0 = be32(be + 0x20), d1 = be32(be + 0x24);
	uint32_t p0 = be32(be + 0x28), p1 = be32(be + 0x2C);
	for (uint32_t o = 0x18; o <= 0x3C; o += 4) sw32(out + o);
	Bounds b;
	b.add(t0); b.add(t1); b.add(d0); b.add(d1); b.add(p0); b.add(p1);
	b.add(be32(be + 0x30)); b.add(be32(be + 0x34));
	b.add(be32(be + 0x38)); b.add(be32(be + 0x3C)); b.seal(size);
	if (t0) swap_run(out, t0, b.end(t0, size), 2);  // J3DAnmColorFullTable u16
	if (t1) swap_run(out, t1, b.end(t1, size), 2);
	if (d0) swap_VtxColorIndexData(out, d0, num0, size);
	if (d1) swap_VtxColorIndexData(out, d1, num1, size);
	if (p0) swap_run(out, p0, b.end(p0, size), 2);  // index pointer u16[]
	if (p1) swap_run(out, p1, b.end(p1, size), 2);
	// R/G/B/A values are u8[] in the FULL family: no swap.
}

// =============================================================================
AnmSwapResult anm_swap_to_host(const uint8_t* be_data, size_t len,
                               std::vector<uint8_t>& out) {
	AnmSwapResult r;
	if (len < 0x24) { r.error = "too small"; return r; }
	out.assign(be_data, be_data + len);

	const uint32_t magic = be32(be_data + 0x00);
	if (magic != 0x4A334431 /* 'J3D1' */) { r.error = "not J3D1"; return r; }
	r.file_type = be32(be_data + 0x04);
	sw32(out.data() + 0x00);   // mMagic
	sw32(out.data() + 0x04);   // mType
	sw32(out.data() + 0x08);   // mFileSize
	sw32(out.data() + 0x0C);   // mBlockNum
	// Header bytes 0x10..0x20 (incl. mSeAnmOffset@0x1C) are not read by the loader
	// — left as-is, matching bmd_swap's handling of the BMD header tail.

	const uint32_t block_num = be32(be_data + 0x0C);
	r.block_num = block_num;

	uint32_t off = 0x20;  // mFirstBlock
	for (uint32_t i = 0; i < block_num; ++i) {
		if (off + 8 > len) { r.error = "block table overrun"; return r; }
		const uint32_t tag = be32(be_data + off + 0);
		const uint32_t bsz = be32(be_data + off + 4);
		if (bsz < 8) { r.error = "bad block size"; return r; }
		sw32(out.data() + off + 0);   // mType (so the loader's fourcc compare matches)
		sw32(out.data() + off + 4);   // mSize

		// A J3D block's declared mSize is padded to alignment and can run a few
		// bytes past the file's mFileSize-cut length (the file-header mFileSize is
		// not authoritative — the in-game loader reads the full archive entry and
		// bounds nothing by it). Clamp the swap working-size to the bytes actually
		// present so the per-block swappers never touch beyond `out`; only trailing
		// alignment padding is dropped, never a referenced region.
		const uint32_t avail = (uint32_t)len - off;
		const uint32_t eff   = bsz < avail ? bsz : avail;

		uint8_t*       obo = out.data() + off;   // output block base
		const uint8_t* bbo = be_data + off;      // big-endian block base
		bool covered = true;
		switch (tag) {
		case 0x414E4B31: /* ANK1 */ swap_ANK1(obo, bbo, eff); break;
		case 0x54544B31: /* TTK1 */ swap_TTK1(obo, bbo, eff); break;
		case 0x50414B31: /* PAK1 */ swap_PAK1(obo, bbo, eff); break;
		case 0x434C4B31: /* CLK1 */ swap_CLK1(obo, bbo, eff); break;
		case 0x54524B31: /* TRK1 */ swap_TRK1(obo, bbo, eff); break;
		case 0x56434B31: /* VCK1 */ swap_VCK1(obo, bbo, eff); break;
		case 0x414E4631: /* ANF1 */ swap_ANF1(obo, bbo, eff); break;
		case 0x50414631: /* PAF1 */ swap_PAF1(obo, bbo, eff); break;
		case 0x54505431: /* TPT1 */ swap_TPT1(obo, bbo, eff); break;
		case 0x434C4631: /* CLF1 */ swap_CLF1(obo, bbo, eff); break;
		case 0x56414631: /* VAF1 */ swap_VAF1(obo, bbo, eff); break;
		case 0x56434631: /* VCF1 */ swap_VCF1(obo, bbo, eff); break;
		default: covered = false; break;
		}
		if (covered) r.blocks_covered++;
		// Advance by the DECLARED size (the next block, if any, starts there); the
		// final block's overhang past `len` simply ends the loop's bounds check.
		off += bsz;
	}

	r.ok = true;
	r.all_covered = (r.blocks_covered == block_num);
	return r;
}

}  // namespace smsport::assets
