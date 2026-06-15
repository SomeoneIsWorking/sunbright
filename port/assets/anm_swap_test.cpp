// Self-contained unit test for the J3D1 animation-file big-endian->host swapper
// (anm_swap). Uses SYNTHETIC in-memory big-endian files (no copyrighted asset):
//   - a 'bck1' with one ANK1 block (the common transform-key animation: keyframe
//     table u16 run + scale f32 / rotation s16 / translate f32 value arrays), and
//   - a 'btp1' with one TPT1 block (the tex-pattern FULL animation, whose table
//     J3DAnmTexPatternFullTable has a MIXED layout — u16/u16/u8/u16 — that must NOT
//     be swapped as a plain u16 run, plus a ResNTAB name table).
// Asserts the file header + block interiors read host-endian after the swap and
// that byte-typed fields (mTexNo) are left untouched. Exits non-zero on any fail.
#include "anm_swap.h"
#include "rarc.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace smsport::assets;

static void put32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
	b[off+0]=v>>24; b[off+1]=v>>16; b[off+2]=v>>8; b[off+3]=v;  // big-endian
}
static void put16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
	b[off+0]=v>>8; b[off+1]=v;
}
static void putf(std::vector<uint8_t>& b, size_t off, float v) {
	uint32_t u; memcpy(&u,&v,4); put32(b,off,u);
}
static uint32_t h32(const uint8_t* p){ uint32_t v; memcpy(&v,p,4); return v; }
static uint16_t h16(const uint8_t* p){ uint16_t v; memcpy(&v,p,2); return v; }
static float    hf (const uint8_t* p){ float v; memcpy(&v,p,4); return v; }

int main() {
	int fail = 0;
	#define CK(c,m) do{ if(!(c)){ printf("FAIL: %s\n",m); fail++; } }while(0)

	// ---- bck1 / ANK1 --------------------------------------------------------
	{
		const uint32_t B = 0x20;                 // block start (mFirstBlock)
		const uint32_t TBL = 0x24, SCL = 0x38, ROT = 0x40, TRN = 0x44;
		const uint32_t BSZ = 0x4C, TOTAL = B + BSZ;
		std::vector<uint8_t> be(TOTAL, 0);
		put32(be, 0x00, 0x4A334431);             // 'J3D1'
		put32(be, 0x04, 0x62636B31);             // 'bck1'
		put32(be, 0x08, TOTAL);                  // fileSize
		put32(be, 0x0C, 1);                      // blockNum
		put32(be, B+0x00, 0x414E4B31);           // 'ANK1'
		put32(be, B+0x04, BSZ);
		be[B+0x08] = 2;                          // mAttribute u8
		be[B+0x09] = 1;                          // mDecShift u8
		put16(be, B+0x0A, 0x0140);               // mFrameMax
		put16(be, B+0x0C, 0x0003);               // field_0xc
		put32(be, B+0x10, 0x11223344);           // field_0x10
		put32(be, B+0x14, TBL); put32(be, B+0x18, SCL);
		put32(be, B+0x1C, ROT); put32(be, B+0x20, TRN);
		// J3DAnmTransformKeyTable = 9 u16; put a marker in the first.
		put16(be, B+TBL+0, 0xABCD);
		putf(be, B+SCL+0, 1.5f);                 // scale f32
		put16(be, B+ROT+0, 0x0102);              // rotation s16
		putf(be, B+TRN+0, -2.25f);               // translate f32

		std::vector<uint8_t> out;
		AnmSwapResult r = anm_swap_to_host(be.data(), TOTAL, out);
		CK(r.ok && r.all_covered, "ANK1 swap ok+covered");
		CK(r.block_num == 1 && r.blocks_covered == 1, "ANK1 block count");
		CK(h32(out.data()+0x00) == 0x4A334431, "ANK1 magic host");
		CK(h32(out.data()+0x0C) == 1, "ANK1 blockNum host");
		CK(h32(out.data()+B) == 0x414E4B31, "ANK1 tag host");
		CK(out[B+0x08] == 2 && out[B+0x09] == 1, "ANK1 u8 fields untouched");
		CK(h16(out.data()+B+0x0A) == 0x0140, "ANK1 mFrameMax host");
		CK(h32(out.data()+B+0x10) == 0x11223344, "ANK1 field_0x10 host");
		CK(h32(out.data()+B+0x14) == TBL, "ANK1 table offset host");
		CK(h16(out.data()+B+TBL) == 0xABCD, "ANK1 table u16 swapped");
		CK(hf (out.data()+B+SCL) == 1.5f, "ANK1 scale f32 swapped");
		CK(h16(out.data()+B+ROT) == 0x0102, "ANK1 rot s16 swapped");
		CK(hf (out.data()+B+TRN) == -2.25f, "ANK1 trans f32 swapped");
	}

	// ---- btp1 / TPT1 (mixed-width table + ResNTAB) --------------------------
	{
		const uint32_t B = 0x20;
		const uint32_t TBL = 0x20, VAL = 0x28, MID = 0x2C, NTB = 0x30;
		// table: one 8-byte J3DAnmTexPatternFullTable; values: 2 u16; matid: 1 u16
		// (pad to 4); nametab: count=1, entry, 2-byte string.
		const uint32_t BSZ = 0x3C, TOTAL = B + BSZ;
		std::vector<uint8_t> be(TOTAL, 0);
		put32(be, 0x00, 0x4A334431);             // 'J3D1'
		put32(be, 0x04, 0x62747031);             // 'btp1'
		put32(be, 0x08, TOTAL);
		put32(be, 0x0C, 1);
		put32(be, B+0x00, 0x54505431);           // 'TPT1'
		put32(be, B+0x04, BSZ);
		put16(be, B+0x0A, 0x0064);               // mFrameMax
		put16(be, B+0x0C, 0x0002);               // field_0xc
		put16(be, B+0x0E, 0x0001);               // field_0xe
		put32(be, B+0x10, TBL); put32(be, B+0x14, VAL);
		put32(be, B+0x18, MID); put32(be, B+0x1C, NTB);
		// table element @TBL: u16 mMaxFrame, u16 mOffset, u8 mTexNo, pad, u16 _6.
		put16(be, B+TBL+0, 0x0010);              // mMaxFrame
		put16(be, B+TBL+2, 0x0007);              // mOffset
		be[B+TBL+4] = 0x5A;                       // mTexNo (u8, MUST stay)
		put16(be, B+TBL+6, 0x1234);              // _6
		put16(be, B+VAL+0, 0x0009);              // mTextureIndex[0]
		put16(be, B+VAL+2, 0x000A);              // mTextureIndex[1]
		put16(be, B+MID+0, 0x0005);              // updateMaterialID[0]
		// ResNTAB @NTB: u16 count=1, u16 pad, {u16 keyCode, u16 offs}, "a\0".
		put16(be, B+NTB+0, 1);                   // mEntryNum
		put16(be, B+NTB+4, 0xBEEF);              // entry keyCode
		put16(be, B+NTB+6, 0x0008);              // entry offs
		be[B+NTB+8] = 'a';

		std::vector<uint8_t> out;
		AnmSwapResult r = anm_swap_to_host(be.data(), TOTAL, out);
		CK(r.ok && r.all_covered, "TPT1 swap ok+covered");
		CK(r.file_type == 0x62747031, "TPT1 file_type host");
		CK(h32(out.data()+B) == 0x54505431, "TPT1 tag host");
		CK(h16(out.data()+B+0x0A) == 0x0064, "TPT1 mFrameMax host");
		CK(h16(out.data()+B+TBL+0) == 0x0010, "TPT1 table mMaxFrame swapped");
		CK(h16(out.data()+B+TBL+2) == 0x0007, "TPT1 table mOffset swapped");
		CK(out[B+TBL+4] == 0x5A, "TPT1 table mTexNo u8 UNTOUCHED");
		CK(h16(out.data()+B+TBL+6) == 0x1234, "TPT1 table _6 swapped");
		CK(h16(out.data()+B+VAL+0) == 0x0009, "TPT1 texIndex[0] swapped");
		CK(h16(out.data()+B+VAL+2) == 0x000A, "TPT1 texIndex[1] swapped");
		CK(h16(out.data()+B+MID+0) == 0x0005, "TPT1 matID swapped");
		CK(h16(out.data()+B+NTB+0) == 1, "TPT1 nametab count swapped");
		CK(h16(out.data()+B+NTB+4) == 0xBEEF, "TPT1 nametab keyCode swapped");
		CK(out[B+NTB+8] == 'a', "TPT1 nametab string untouched");
	}

	// ---- uncovered block -> all_covered=false (clean refusal contract) ------
	{
		const uint32_t B = 0x20, BSZ = 0x10, TOTAL = B + BSZ;
		std::vector<uint8_t> be(TOTAL, 0);
		put32(be, 0x00, 0x4A334431);             // 'J3D1'
		put32(be, 0x04, 0x62786B31);             // 'bxk1' (we still cover VCK1...)
		put32(be, 0x08, TOTAL);
		put32(be, 0x0C, 1);
		put32(be, B+0x00, 0x5A5A5A5A);           // unknown block tag
		put32(be, B+0x04, BSZ);
		std::vector<uint8_t> out;
		AnmSwapResult r = anm_swap_to_host(be.data(), TOTAL, out);
		CK(r.ok && !r.all_covered, "unknown block -> ok but not all_covered");
	}

	if (fail == 0) printf("anm_swap_test: ALL PASS\n");
	return fail ? 1 : 0;
}
