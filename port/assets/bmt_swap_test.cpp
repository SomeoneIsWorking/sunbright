// Self-contained unit test for the BMT (.bmt material-table) big-endian->host
// swapper. Uses a SYNTHETIC in-memory big-endian bmt3 (no copyrighted asset)
// holding {MAT3, TEX1} — the block shape of every real SMS .bmt. Asserts the
// header reads host-endian after swap and that the MAT3/TEX1 interiors are
// swapped (these block swappers are the proven ones shared with bmd_swap, so the
// focus here is the bmt header/dispatch). Exits non-zero on any failed check.
#include "bmt_swap.h"
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

int main() {
	int fail = 0;
	#define CK(c,m) do{ if(!(c)){ printf("FAIL: %s\n",m); fail++; } }while(0)

	// bmt3 with two blocks: MAT3 then TEX1.
	const uint32_t MAT3_OFF = 0x20, MAT3_SIZE = 0x264;
	const uint32_t TEX1_OFF = MAT3_OFF + MAT3_SIZE, TEX1_SIZE = 0x40;
	const uint32_t TOTAL = TEX1_OFF + TEX1_SIZE;
	std::vector<uint8_t> be(TOTAL, 0);
	put32(be, 0x00, 0x4A334432);          // 'J3D2'
	put32(be, 0x04, 0x626D7433);          // 'bmt3'
	put32(be, 0x08, TOTAL);               // fileSize
	put32(be, 0x0C, 2);                   // blockNum = 2

	// MAT3 block: matNum=1; initData@0x84 (stride 0x14C), matID@0x1d0,
	//   texMtxInfo@0x1d4 (0x64), fogInfo@0x238 (0x2C).
	const uint32_t MAT_INIT=0x84, MAT_ID=0x1d0, MAT_TEXMTX=0x1d4, MAT_FOG=0x238;
	put32(be, MAT3_OFF+0x00, 0x4D415433); // 'MAT3'
	put32(be, MAT3_OFF+0x04, MAT3_SIZE);
	put16(be, MAT3_OFF+0x08, 0x0001);     // mMaterialNum
	put32(be, MAT3_OFF+0x0C+0*4, MAT_INIT);
	put32(be, MAT3_OFF+0x0C+1*4, MAT_ID);
	put32(be, MAT3_OFF+0x0C+13*4, MAT_TEXMTX);
	put32(be, MAT3_OFF+0x0C+23*4, MAT_FOG);
	be[MAT3_OFF+MAT_INIT+0x00]=5;             // mMaterialMode u8 (no swap)
	put16(be, MAT3_OFF+MAT_INIT+0x008, 0x0102); // mMatColorIdx[0]
	put16(be, MAT3_OFF+MAT_INIT+0x144, 0x0003); // mFogIdx
	put16(be, MAT3_OFF+MAT_ID, 0x0000);       // matID[0]
	be[MAT3_OFF+MAT_TEXMTX+0x00]=1;           // mProjection u8
	putf(be, MAT3_OFF+MAT_TEXMTX+0x04, 0.5f); // mCenter.x
	put16(be, MAT3_OFF+MAT_TEXMTX+0x18, 0x4000); // SRT mRotation s16
	put16(be, MAT3_OFF+MAT_FOG+0x02, 0x0080); // fog mCenter
	putf(be, MAT3_OFF+MAT_FOG+0x10, 5000.0f); // fog mFarZ

	// TEX1 block: texNum=1; ResTIMG @0x20 (stride 0x20).
	const uint32_t TEX_RES=0x20;
	put32(be, TEX1_OFF+0x00, 0x54455831); // 'TEX1'
	put32(be, TEX1_OFF+0x04, TEX1_SIZE);
	put16(be, TEX1_OFF+0x08, 0x0001);     // mTextureNum
	put32(be, TEX1_OFF+0x0C, TEX_RES);    // mpTextureRes
	put32(be, TEX1_OFF+0x10, 0x00000000); // mpNameTable = 0
	be[TEX1_OFF+TEX_RES+0x00]=14;             // format=CMPR u8 (no swap)
	put16(be, TEX1_OFF+TEX_RES+0x02, 0x0040); // width=64
	put16(be, TEX1_OFF+TEX_RES+0x04, 0x0020); // height=32
	put32(be, TEX1_OFF+TEX_RES+0x1C, 0x00000040); // imageDataOffset

	std::vector<uint8_t> out;
	BmtSwapResult r = bmt_swap_to_host(be.data(), be.size(), out);

	CK(r.ok, "swap ok");
	CK(r.error == nullptr, "no error");
	CK(r.block_num == 2, "block_num == 2");
	CK(r.blocks_covered == 2 && r.all_covered, "MAT3+TEX1 covered + all_covered");

	// Header reads host-endian after swap.
	CK(h32(out.data()+0x00)==0x4A334432, "magic 'J3D2' host-readable");
	CK(h32(out.data()+0x04)==0x626D7433, "type 'bmt3' host-readable");
	CK(h32(out.data()+0x08)==TOTAL,      "fileSize host-readable");
	CK(h32(out.data()+0x0C)==2,          "blockNum host-readable");
	CK(h32(out.data()+MAT3_OFF)==0x4D415433, "MAT3 tag host-readable");
	CK(h32(out.data()+TEX1_OFF)==0x54455831, "TEX1 tag host-readable");

	// MAT3 interior swapped (delegates to the shared swapper).
	CK(h16(out.data()+MAT3_OFF+0x08)==0x0001, "MAT3 mMaterialNum");
	CK(out[MAT3_OFF+MAT_INIT+0x00]==5, "MAT3 mMaterialMode u8 untouched");
	CK(h16(out.data()+MAT3_OFF+MAT_INIT+0x008)==0x0102, "MAT3 mMatColorIdx");
	CK(h16(out.data()+MAT3_OFF+MAT_INIT+0x144)==0x0003, "MAT3 mFogIdx");
	CK(out[MAT3_OFF+MAT_TEXMTX+0x00]==1, "MAT3 texMtx mProjection u8 untouched");
	{ float cx; memcpy(&cx,out.data()+MAT3_OFF+MAT_TEXMTX+0x04,4); CK(cx==0.5f,"MAT3 texMtx center.x"); }
	CK(h16(out.data()+MAT3_OFF+MAT_TEXMTX+0x18)==0x4000,"MAT3 texMtx SRT rotation s16");
	{ float fz; memcpy(&fz,out.data()+MAT3_OFF+MAT_FOG+0x10,4); CK(fz==5000.0f,"MAT3 fog mFarZ"); }

	// TEX1 interior swapped.
	CK(h16(out.data()+TEX1_OFF+0x08)==0x0001, "TEX1 mTextureNum");
	CK(out[TEX1_OFF+TEX_RES+0x00]==14, "TEX1 format u8 untouched");
	CK(h16(out.data()+TEX1_OFF+TEX_RES+0x02)==0x0040, "TEX1 width");
	CK(h16(out.data()+TEX1_OFF+TEX_RES+0x04)==0x0020, "TEX1 height");
	CK(h32(out.data()+TEX1_OFF+TEX_RES+0x1C)==0x00000040, "TEX1 imageDataOffset");

	// Rejection paths.
	{ std::vector<uint8_t> o2; auto bad = be; put32(bad,0x04,0x626D6433/*'bmd3'*/);
	  BmtSwapResult rr = bmt_swap_to_host(bad.data(), bad.size(), o2);
	  CK(!rr.ok && rr.error != nullptr, "rejects non-bmt type"); }

	if (fail) { printf("*** %d FAILURE(S) ***\n", fail); return 1; }
	printf("bmt_swap_test: ALL PASS\n");
	return 0;
}
