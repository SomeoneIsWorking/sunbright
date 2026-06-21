// anm_swap_test.cpp — verify-first unit test for the native BE->host J3D1
// animation-file swap (native/assets/anm_swap.cpp, anm_swap_to_host).
//
// Builds a synthetic BIG-ENDIAN .bck (ANK1) and .btp (TPT1) blob, runs the
// shipping swapper, then reads each field back AT THE SAME OFFSET the decomp
// loader (J3DAnmLoader.cpp) reads it and asserts it equals the hand-derived host
// value. Also checks coverage accounting (all_covered) and bad-magic no-op.
//
// The swapper IS the shipping function wired into J3DAnmLoaderDataBase::load.
// No GPU / no ROM / pure byte logic.

#include "anm_swap.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); g_fail = 1; } \
	else         { std::fprintf(stderr, "ok:   %s\n", msg); } \
} while (0)

// --- big-endian writers into the synthetic on-disk blob ----------------------
static void put_be16(std::vector<uint8_t>& p, uint32_t off, uint16_t v) {
	p[off] = (uint8_t)(v >> 8); p[off + 1] = (uint8_t)v;
}
static void put_be32(std::vector<uint8_t>& p, uint32_t off, uint32_t v) {
	p[off] = (uint8_t)(v >> 24); p[off + 1] = (uint8_t)(v >> 16);
	p[off + 2] = (uint8_t)(v >> 8); p[off + 3] = (uint8_t)v;
}
static void put_be_f32(std::vector<uint8_t>& p, uint32_t off, float f) {
	uint32_t u; std::memcpy(&u, &f, 4); put_be32(p, off, u);
}
static void put_tag(std::vector<uint8_t>& p, uint32_t off, const char* s) {
	p[off] = s[0]; p[off + 1] = s[1]; p[off + 2] = s[2]; p[off + 3] = s[3];
}
// --- host readers (what the loader sees AFTER the swap) ----------------------
static uint16_t host_u16(const std::vector<uint8_t>& p, uint32_t off) {
	uint16_t v; std::memcpy(&v, p.data() + off, 2); return v;
}
static int16_t host_s16(const std::vector<uint8_t>& p, uint32_t off) {
	int16_t v; std::memcpy(&v, p.data() + off, 2); return v;
}
static uint32_t host_u32(const std::vector<uint8_t>& p, uint32_t off) {
	uint32_t v; std::memcpy(&v, p.data() + off, 4); return v;
}
static float host_f32(const std::vector<uint8_t>& p, uint32_t off) {
	float v; std::memcpy(&v, p.data() + off, 4); return v;
}

// ============================================================================
// ANK1 (.bck transform-key): one block, count=1 track. Layout (block @0x20):
//   header to 0x24; table @0x24 (9 u16 = 0x12); scale @0x36 (2 f32);
//   rot @0x3E (2 s16); trans @0x42 (2 f32); block end @0x4A.
static void test_ank1() {
	const uint32_t B = 0x20;              // block base
	const uint32_t off_tab = 0x24, off_scale = 0x36, off_rot = 0x3E,
	               off_trans = 0x42, bsz = 0x4A;
	std::vector<uint8_t> be(B + bsz, 0);
	// file header
	put_tag(be, 0x00, "J3D1");
	put_tag(be, 0x04, "bck1");
	put_be32(be, 0x08, B + bsz);          // mFileSize
	put_be32(be, 0x0C, 1);                // mBlockNum
	// block header + fields
	put_tag(be, B + 0x00, "ANK1");
	put_be32(be, B + 0x04, bsz);
	be[B + 0x08] = 0x01;                  // mAttribute (u8)
	be[B + 0x09] = 0x05;                  // mDecShift  (u8)
	put_be16(be, B + 0x0A, (uint16_t)1234);     // mFrameMax
	put_be16(be, B + 0x0C, 1);            // field_0xc (count)
	put_be32(be, B + 0x10, 0xAABBCCDD);   // field_0x10
	put_be32(be, B + 0x14, off_tab);
	put_be32(be, B + 0x18, off_scale);
	put_be32(be, B + 0x1C, off_rot);
	put_be32(be, B + 0x20, off_trans);
	// table: 9 u16 (mScale{max,off,type}, mRotation{...}, mTranslate{...})
	for (int i = 0; i < 9; ++i) put_be16(be, B + off_tab + i * 2, (uint16_t)(0x1000 + i));
	// scale f32[2], rot s16[2], trans f32[2]
	put_be_f32(be, B + off_scale + 0, 1.5f);
	put_be_f32(be, B + off_scale + 4, -2.25f);
	put_be16(be, B + off_rot + 0, (uint16_t)0x7F00);
	put_be16(be, B + off_rot + 2, (uint16_t)0x8001);
	put_be_f32(be, B + off_trans + 0, 100.0f);
	put_be_f32(be, B + off_trans + 4, -0.5f);

	std::vector<uint8_t> out;
	auto r = smsport::assets::anm_swap_to_host(be.data(), be.size(), out);
	CHECK(r.ok && r.all_covered, "ANK1: ok && all_covered");
	CHECK(r.block_num == 1 && r.blocks_covered == 1, "ANK1: 1/1 blocks covered");
	// file header swapped to host
	CHECK(host_u32(out, 0x00) == 'J3D1', "ANK1: magic -> host 'J3D1'");
	CHECK(host_u32(out, 0x04) == 'bck1', "ANK1: type -> host 'bck1'");
	CHECK(host_u32(out, 0x0C) == 1, "ANK1: blockNum host 1");
	// block header + fields
	CHECK(host_u32(out, B + 0x00) == 'ANK1', "ANK1: block tag -> host 'ANK1'");
	CHECK(host_u32(out, B + 0x04) == bsz, "ANK1: block size host");
	CHECK(out[B + 0x08] == 0x01 && out[B + 0x09] == 0x05, "ANK1: u8 attr/decShift untouched");
	CHECK(host_s16(out, B + 0x0A) == 1234, "ANK1: mFrameMax host");
	CHECK(host_u16(out, B + 0x0C) == 1, "ANK1: field_0xc count host");
	CHECK(host_u32(out, B + 0x10) == 0xAABBCCDD, "ANK1: field_0x10 host");
	CHECK(host_u32(out, B + 0x14) == off_tab, "ANK1: table offset host");
	// table u16s
	bool tab_ok = true;
	for (int i = 0; i < 9; ++i)
		if (host_u16(out, B + off_tab + i * 2) != (uint16_t)(0x1000 + i)) tab_ok = false;
	CHECK(tab_ok, "ANK1: 9 table u16 host");
	// value arrays
	CHECK(host_f32(out, B + off_scale + 0) == 1.5f
	   && host_f32(out, B + off_scale + 4) == -2.25f, "ANK1: scale f32 host");
	CHECK(host_u16(out, B + off_rot + 0) == 0x7F00
	   && host_u16(out, B + off_rot + 2) == 0x8001, "ANK1: rot s16 host");
	CHECK(host_f32(out, B + off_trans + 0) == 100.0f
	   && host_f32(out, B + off_trans + 4) == -0.5f, "ANK1: trans f32 host");
}

// ============================================================================
// TPT1 (.btp texpattern-full): one block, count=2 materials. Layout (block @0x20):
//   header to 0x20; table @0x20 (2 x 8); vals @0x30 (3 u16); matid @0x36 (2 u16);
//   nameTab @0x3A (ResNTAB: 1 entry); block end @0x46.
static void test_tpt1() {
	const uint32_t B = 0x20;
	const uint32_t off_tab = 0x20, off_vals = 0x30, off_matid = 0x36,
	               off_name = 0x3A;
	// ResNTAB @off_name: u16 entryNum=1, u16 pad, {u16 key, u16 strOff}, "tex\0"
	const uint32_t name_stroff = 8;       // string starts 8 bytes into the table
	const uint32_t bsz = off_name + name_stroff + 4 /*"tex\0"*/;
	std::vector<uint8_t> be(B + bsz, 0);
	put_tag(be, 0x00, "J3D1");
	put_tag(be, 0x04, "btp1");
	put_be32(be, 0x08, B + bsz);
	put_be32(be, 0x0C, 1);                // mBlockNum
	put_tag(be, B + 0x00, "TPT1");
	put_be32(be, B + 0x04, bsz);
	be[B + 0x08] = 0x02; be[B + 0x09] = 0x00;
	put_be16(be, B + 0x0A, 60);           // mFrameMax
	put_be16(be, B + 0x0C, 2);            // field_0xc (count = mUpdateMaterialNum)
	put_be16(be, B + 0x0E, 3);            // field_0xe
	put_be32(be, B + 0x10, off_tab);
	put_be32(be, B + 0x14, off_vals);
	put_be32(be, B + 0x18, off_matid);
	put_be32(be, B + 0x1C, off_name);
	// table: 2 entries {u16 maxFrame, u16 offset, u8 texNo, pad, u16 _6}
	put_be16(be, B + off_tab + 0x00, 10); put_be16(be, B + off_tab + 0x02, 0);
	be[B + off_tab + 0x04] = 0x07;        // mTexNo u8
	put_be16(be, B + off_tab + 0x06, 0x1111);
	put_be16(be, B + off_tab + 0x08, 20); put_be16(be, B + off_tab + 0x0A, 3);
	be[B + off_tab + 0x0C] = 0x09;
	put_be16(be, B + off_tab + 0x0E, 0x2222);
	// vals: 3 u16 texture indices
	put_be16(be, B + off_vals + 0, 100);
	put_be16(be, B + off_vals + 2, 101);
	put_be16(be, B + off_vals + 4, 102);
	// matid: 2 u16
	put_be16(be, B + off_matid + 0, 0xABCD);
	put_be16(be, B + off_matid + 2, 0x1234);
	// ResNTAB
	put_be16(be, B + off_name + 0, 1);    // entryNum
	put_be16(be, B + off_name + 2, 0);    // pad
	put_be16(be, B + off_name + 4, 0xBEEF);   // keyCode
	put_be16(be, B + off_name + 6, name_stroff);  // strOff
	std::memcpy(be.data() + B + off_name + name_stroff, "tex", 4);  // "tex\0"

	std::vector<uint8_t> out;
	auto r = smsport::assets::anm_swap_to_host(be.data(), be.size(), out);
	CHECK(r.ok && r.all_covered, "TPT1: ok && all_covered");
	CHECK(r.block_num == 1 && r.blocks_covered == 1, "TPT1: 1/1 blocks covered");
	CHECK(host_u32(out, B + 0x00) == 'TPT1', "TPT1: block tag host");
	CHECK(host_s16(out, B + 0x0A) == 60, "TPT1: mFrameMax host");
	CHECK(host_u16(out, B + 0x0C) == 2, "TPT1: field_0xc host");
	CHECK(host_u16(out, B + 0x0E) == 3, "TPT1: field_0xe host");
	CHECK(host_u32(out, B + 0x10) == off_tab, "TPT1: table offset host");
	// table entries
	CHECK(host_u16(out, B + off_tab + 0x00) == 10
	   && host_u16(out, B + off_tab + 0x02) == 0
	   && out[B + off_tab + 0x04] == 0x07
	   && host_u16(out, B + off_tab + 0x06) == 0x1111, "TPT1: table entry 0 host");
	CHECK(host_u16(out, B + off_tab + 0x08) == 20
	   && host_u16(out, B + off_tab + 0x0A) == 3
	   && out[B + off_tab + 0x0C] == 0x09
	   && host_u16(out, B + off_tab + 0x0E) == 0x2222, "TPT1: table entry 1 host");
	// texture index values
	CHECK(host_u16(out, B + off_vals + 0) == 100
	   && host_u16(out, B + off_vals + 2) == 101
	   && host_u16(out, B + off_vals + 4) == 102, "TPT1: 3 texture-index u16 host");
	// material IDs
	CHECK(host_u16(out, B + off_matid + 0) == 0xABCD
	   && host_u16(out, B + off_matid + 2) == 0x1234, "TPT1: matID u16 host");
	// name table
	CHECK(host_u16(out, B + off_name + 0) == 1, "TPT1: ResNTAB entryNum host");
	CHECK(host_u16(out, B + off_name + 4) == 0xBEEF, "TPT1: ResNTAB keyCode host");
	CHECK(host_u16(out, B + off_name + 6) == name_stroff, "TPT1: ResNTAB strOff host");
	CHECK(std::memcmp(out.data() + B + off_name + name_stroff, "tex", 4) == 0,
	      "TPT1: ResNTAB string blob untouched");
}

// ============================================================================
// TTK1 (.btk texture-SRT key): one block. Header is 0x60; lay arrays after it:
//   table @0x60 (mTrackNum=1 -> 9 u16 = 0x12); matid @0x72 (1 u16);
//   nameTab1 @0x74 (1 entry, str@8 "tx"); scale @0x84 (2 f32); rot @0x8C (2 s16);
//   trans @0x90 (2 f32). (Other offsets 0 to keep the fixture small.)
static void test_ttk1() {
	const uint32_t B = 0x20;
	const uint32_t off_tab = 0x60, off_matid = 0x72, off_name = 0x74,
	               off_scale = 0x84, off_rot = 0x8C, off_trans = 0x90;
	const uint32_t name_stroff = 8;
	const uint32_t bsz = off_trans + 8;        // 2 f32
	std::vector<uint8_t> be(B + bsz, 0);
	put_tag(be, 0x00, "J3D1");
	put_tag(be, 0x04, "btk1");
	put_be32(be, 0x08, B + bsz);
	put_be32(be, 0x0C, 1);
	put_tag(be, B + 0x00, "TTK1");
	put_be32(be, B + 0x04, bsz);
	be[B + 0x08] = 0x01; be[B + 0x09] = 0x05;  // attribute / decShift (u8)
	put_be16(be, B + 0x0A, 240);               // mMaxFrame
	put_be16(be, B + 0x0C, 1);                 // mTrackNum
	put_be16(be, B + 0x0E, 2);                 // mScaleNum
	put_be16(be, B + 0x10, 2);                 // mRotNum
	put_be16(be, B + 0x12, 2);                 // mTransNum
	put_be32(be, B + 0x14, off_tab);
	put_be32(be, B + 0x18, off_matid);
	put_be32(be, B + 0x1C, off_name);
	// 0x20 texMtxID, 0x24 SRTCenter left 0
	put_be32(be, B + 0x28, off_scale);
	put_be32(be, B + 0x2C, off_rot);
	put_be32(be, B + 0x30, off_trans);
	// post-block offsets (0x3C..0x58) all 0
	// table: 9 u16
	for (int i = 0; i < 9; ++i) put_be16(be, B + off_tab + i * 2, (uint16_t)(0x2000 + i));
	put_be16(be, B + off_matid, 0x55AA);       // updateMaterialID
	// nameTab1 ResNTAB (1 entry)
	put_be16(be, B + off_name + 0, 1);
	put_be16(be, B + off_name + 4, 0xCAFE);
	put_be16(be, B + off_name + 6, name_stroff);
	std::memcpy(be.data() + B + off_name + name_stroff, "tx", 3);
	put_be_f32(be, B + off_scale + 0, 2.0f);
	put_be_f32(be, B + off_scale + 4, 0.25f);
	put_be16(be, B + off_rot + 0, (uint16_t)0x4000);
	put_be16(be, B + off_rot + 2, (uint16_t)0xC000);
	put_be_f32(be, B + off_trans + 0, 5.0f);
	put_be_f32(be, B + off_trans + 4, -3.0f);

	std::vector<uint8_t> out;
	auto r = smsport::assets::anm_swap_to_host(be.data(), be.size(), out);
	CHECK(r.ok && r.all_covered, "TTK1: ok && all_covered");
	CHECK(host_u32(out, B + 0x00) == 'TTK1', "TTK1: block tag host");
	CHECK(host_s16(out, B + 0x0A) == 240, "TTK1: mMaxFrame host");
	CHECK(host_u16(out, B + 0x0C) == 1 && host_u16(out, B + 0x0E) == 2
	   && host_u16(out, B + 0x10) == 2 && host_u16(out, B + 0x12) == 2,
	      "TTK1: track/scale/rot/trans counts host");
	CHECK(host_u32(out, B + 0x14) == off_tab && host_u32(out, B + 0x30) == off_trans,
	      "TTK1: offsets host");
	bool tab_ok = true;
	for (int i = 0; i < 9; ++i)
		if (host_u16(out, B + off_tab + i * 2) != (uint16_t)(0x2000 + i)) tab_ok = false;
	CHECK(tab_ok, "TTK1: 9 table u16 host");
	CHECK(host_u16(out, B + off_matid) == 0x55AA, "TTK1: updateMaterialID host");
	CHECK(host_u16(out, B + off_name + 0) == 1
	   && host_u16(out, B + off_name + 4) == 0xCAFE
	   && host_u16(out, B + off_name + 6) == name_stroff, "TTK1: nameTab1 host");
	CHECK(std::memcmp(out.data() + B + off_name + name_stroff, "tx", 3) == 0,
	      "TTK1: nameTab1 string untouched");
	CHECK(host_f32(out, B + off_scale + 0) == 2.0f
	   && host_f32(out, B + off_scale + 4) == 0.25f, "TTK1: scale f32 host");
	CHECK(host_u16(out, B + off_rot + 0) == 0x4000
	   && host_u16(out, B + off_rot + 2) == 0xC000, "TTK1: rot s16 host");
	CHECK(host_f32(out, B + off_trans + 0) == 5.0f
	   && host_f32(out, B + off_trans + 4) == -3.0f, "TTK1: trans f32 host");
}

// ============================================================================
// TRK1 (.brk TEV-register key): one block, cregNum=1 kregNum=1. Header 0x58.
//   CRegTable @0x58 (1 x 0x1C); KRegTable @0x74 (1 x 0x1C); CRegMatID @0x90 (1 u16);
//   CRValues @0x92 (2 s16); block end @0x96. (Other offsets 0.)
static void test_trk1() {
	const uint32_t B = 0x20;
	const uint32_t off_cregtab = 0x58, off_kregtab = 0x74, off_cmatid = 0x90,
	               off_crval = 0x92;
	const uint32_t bsz = off_crval + 4;        // 2 s16
	std::vector<uint8_t> be(B + bsz, 0);
	put_tag(be, 0x00, "J3D1");
	put_tag(be, 0x04, "brk1");
	put_be32(be, 0x08, B + bsz);
	put_be32(be, 0x0C, 1);
	put_tag(be, B + 0x00, "TRK1");
	put_be32(be, B + 0x04, bsz);
	put_be16(be, B + 0x0A, 90);                // mFrameMax
	put_be16(be, B + 0x0C, 1);                 // mCRegUpdateMaterialNum
	put_be16(be, B + 0x0E, 1);                 // mKRegUpdateMaterialNum
	put_be16(be, B + 0x10, 2);                 // CReg R count
	put_be32(be, B + 0x20, off_cregtab);
	put_be32(be, B + 0x24, off_kregtab);
	put_be32(be, B + 0x28, off_cmatid);
	put_be32(be, B + 0x38, off_crval);         // CR values
	// CRegTable entry: 12 u16 then u8 colorId + pad
	for (int i = 0; i < 12; ++i) put_be16(be, B + off_cregtab + i * 2, (uint16_t)(0x3000 + i));
	be[B + off_cregtab + 0x18] = 0x02;         // mColorId u8
	// KRegTable entry
	for (int i = 0; i < 12; ++i) put_be16(be, B + off_kregtab + i * 2, (uint16_t)(0x4000 + i));
	be[B + off_kregtab + 0x18] = 0x03;
	put_be16(be, B + off_cmatid, 0x77EE);      // CReg matID
	put_be16(be, B + off_crval + 0, (uint16_t)0x0102);   // CR s16 values
	put_be16(be, B + off_crval + 2, (uint16_t)0xFFFE);

	std::vector<uint8_t> out;
	auto r = smsport::assets::anm_swap_to_host(be.data(), be.size(), out);
	CHECK(r.ok && r.all_covered, "TRK1: ok && all_covered");
	CHECK(host_u32(out, B + 0x00) == 'TRK1', "TRK1: block tag host");
	CHECK(host_s16(out, B + 0x0A) == 90, "TRK1: mFrameMax host");
	CHECK(host_u16(out, B + 0x0C) == 1 && host_u16(out, B + 0x0E) == 1, "TRK1: reg counts host");
	bool ct = true, kt = true;
	for (int i = 0; i < 12; ++i) {
		if (host_u16(out, B + off_cregtab + i * 2) != (uint16_t)(0x3000 + i)) ct = false;
		if (host_u16(out, B + off_kregtab + i * 2) != (uint16_t)(0x4000 + i)) kt = false;
	}
	CHECK(ct && out[B + off_cregtab + 0x18] == 0x02, "TRK1: CRegTable 12 u16 + colorId u8");
	CHECK(kt && out[B + off_kregtab + 0x18] == 0x03, "TRK1: KRegTable 12 u16 + colorId u8");
	CHECK(host_u16(out, B + off_cmatid) == 0x77EE, "TRK1: CReg matID host");
	CHECK(host_u16(out, B + off_crval + 0) == 0x0102
	   && host_u16(out, B + off_crval + 2) == 0xFFFE, "TRK1: CR s16 values host");
}

// PAF1 (.bpa color full): u8 value arrays (full uses u8, not s16) must stay BE.
static void test_paf1() {
	const uint32_t B = 0x20;
	const uint32_t off_tab = 0x34, off_matid = 0x44, off_name = 0x46,
	               off_r = 0x50;
	const uint32_t name_stroff = 8;
	const uint32_t bsz = off_r + 4;            // 4 u8 R values (then G/B/A omitted/0)
	std::vector<uint8_t> be(B + bsz, 0);
	put_tag(be, 0x00, "J3D1"); put_tag(be, 0x04, "bpa1");
	put_be32(be, 0x08, B + bsz); put_be32(be, 0x0C, 1);
	put_tag(be, B + 0x00, "PAF1"); put_be32(be, B + 0x04, bsz);
	put_be16(be, B + 0x0C, 30);                // mFrameMax
	put_be16(be, B + 0x0E, 1);                 // mUpdateMaterialNum
	put_be32(be, B + 0x18, off_tab);
	put_be32(be, B + 0x1C, off_matid);
	put_be32(be, B + 0x20, off_name);
	put_be32(be, B + 0x24, off_r);             // R values (u8)
	for (int i = 0; i < 8; ++i) put_be16(be, B + off_tab + i * 2, (uint16_t)(0x5000 + i)); // J3DAnmColorFullTable 8 u16
	put_be16(be, B + off_matid, 0x1234);
	put_be16(be, B + off_name + 0, 1);
	put_be16(be, B + off_name + 4, 0xABCD);
	put_be16(be, B + off_name + 6, name_stroff);
	std::memcpy(be.data() + B + off_name + name_stroff, "m", 2);
	be[B + off_r + 0] = 0x11; be[B + off_r + 1] = 0x22;
	be[B + off_r + 2] = 0x33; be[B + off_r + 3] = 0x44;

	std::vector<uint8_t> out;
	auto r = smsport::assets::anm_swap_to_host(be.data(), be.size(), out);
	CHECK(r.ok && r.all_covered, "PAF1: ok && all_covered");
	CHECK(host_s16(out, B + 0x0C) == 30, "PAF1: mFrameMax host");
	bool tab_ok = true;
	for (int i = 0; i < 8; ++i)
		if (host_u16(out, B + off_tab + i * 2) != (uint16_t)(0x5000 + i)) tab_ok = false;
	CHECK(tab_ok, "PAF1: table 8 u16 host");
	CHECK(host_u16(out, B + off_matid) == 0x1234, "PAF1: matID host");
	CHECK(out[B + off_r + 0] == 0x11 && out[B + off_r + 1] == 0x22
	   && out[B + off_r + 2] == 0x33 && out[B + off_r + 3] == 0x44,
	      "PAF1: u8 R values left untouched (full = u8, not s16)");
}

// ============================================================================
static void test_negatives() {
	// bad magic -> no-op (ok=false), out left as-is/unswapped.
	std::vector<uint8_t> be(0x40, 0);
	put_tag(be, 0x00, "BMD2");            // wrong magic
	put_be32(be, 0x0C, 1);
	std::vector<uint8_t> out;
	auto r = smsport::assets::anm_swap_to_host(be.data(), be.size(), out);
	CHECK(!r.ok && r.error != nullptr, "neg: bad magic -> ok=false with error");

	// unhandled block -> all_covered=false (loader OSPanics on this).
	const uint32_t B = 0x20, bsz = 0x10;
	std::vector<uint8_t> be2(B + bsz, 0);
	put_tag(be2, 0x00, "J3D1");
	put_tag(be2, 0x04, "bxk1");
	put_be32(be2, 0x0C, 1);
	put_tag(be2, B + 0x00, "VCK1");       // vtx-color key: deferred / unimplemented
	put_be32(be2, B + 0x04, bsz);
	std::vector<uint8_t> out2;
	auto r2 = smsport::assets::anm_swap_to_host(be2.data(), be2.size(), out2);
	CHECK(r2.ok && !r2.all_covered && r2.blocks_covered == 0
	   && r2.first_uncovered_tag == 0x56434B31,
	      "neg: unhandled VCK1 -> ok but all_covered=false (FAIL FAST)");
}

int main() {
	test_ank1();
	test_tpt1();
	test_ttk1();
	test_trk1();
	test_paf1();
	test_negatives();
	if (g_fail) { std::fprintf(stderr, "\nANM SWAP TEST: FAILED\n"); return 1; }
	std::fprintf(stderr, "\nANM SWAP TEST: PASSED\n");
	return 0;
}
