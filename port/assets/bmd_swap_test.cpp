// Self-contained unit test for the BMD big-endian->host swapper. Uses a
// SYNTHETIC in-memory big-endian BMD (no copyrighted asset needed), mirroring
// the real header+block-table+INF1 layout verified against a real file
// (halfwhiteball.bmd) during development. Exits non-zero on any failed check.
#include "bmd_swap.h"
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
static uint32_t h32(const uint8_t* p){ uint32_t v; memcpy(&v,p,4); return v; }
static uint16_t h16(const uint8_t* p){ uint16_t v; memcpy(&v,p,2); return v; }

static void putf(std::vector<uint8_t>& b, size_t off, float v) {
	uint32_t u; memcpy(&u,&v,4); put32(b,off,u);  // store big-endian
}

int main() {
	int fail = 0;
	#define CK(c,m) do{ if(!(c)){ printf("FAIL: %s\n",m); fail++; } }while(0)

	// Build a synthetic big-endian BMD with 5 blocks. EVP1 precedes JNT1 so the
	// joint-count pre-pass (EVP1's inverse-bind matrix count = JNT1 jointNum) is
	// exercised: INF1 + VTX1 + EVP1 + DRW1 + JNT1.
	const uint32_t INF1_OFF = 0x20, INF1_SIZE = 0x28;
	const uint32_t VTX1_OFF = INF1_OFF + INF1_SIZE, VTX1_SIZE = 0x80;
	const uint32_t EVP1_OFF = VTX1_OFF + VTX1_SIZE, EVP1_SIZE = 0x70;
	const uint32_t DRW1_OFF = EVP1_OFF + EVP1_SIZE, DRW1_SIZE = 0x20;
	const uint32_t JNT1_OFF = DRW1_OFF + DRW1_SIZE, JNT1_SIZE = 0x60;
	const uint32_t TOTAL = JNT1_OFF + JNT1_SIZE;
	std::vector<uint8_t> be(TOTAL, 0);
	put32(be, 0x00, 0x4A334432);          // 'J3D2'
	put32(be, 0x04, 0x626D6433);          // 'bmd3'
	put32(be, 0x08, TOTAL);               // fileSize
	put32(be, 0x0C, 5);                   // blockNum = 5
	// INF1 block
	put32(be, INF1_OFF+0x00, 0x494E4631); // 'INF1'
	put32(be, INF1_OFF+0x04, INF1_SIZE);  // block size
	put16(be, INF1_OFF+0x08, 0x0007);     // mFlags
	put32(be, INF1_OFF+0x0C, 0x00000003); // mPacketNum
	put32(be, INF1_OFF+0x10, 0x00000011); // mVtxNum = 17
	put32(be, INF1_OFF+0x14, 0x00000018); // mpHierarchy offset (block-relative)
	// hierarchy entries at block+0x18: {type,value} pairs, terminated by type=0
	put16(be, INF1_OFF+0x18, 0x0010); put16(be, INF1_OFF+0x1A, 0x0000);  // joint #0
	put16(be, INF1_OFF+0x1C, 0x0011); put16(be, INF1_OFF+0x1E, 0x0001);  // material #1
	put16(be, INF1_OFF+0x20, 0x0000); put16(be, INF1_OFF+0x22, 0x0000);  // terminator
	// VTX1 block: fmt list @0x40 (POS f32 entry + NULL), pos f32 array @0x60.
	const uint32_t VTX_FMT=0x40, VTX_POS=0x60;
	put32(be, VTX1_OFF+0x00, 0x56545831); // 'VTX1'
	put32(be, VTX1_OFF+0x04, VTX1_SIZE);
	put32(be, VTX1_OFF+0x08, VTX_FMT);    // mpVtxAttrFmtList
	put32(be, VTX1_OFF+0x0C, VTX_POS);    // mpVtxPosArray
	// (other array offsets 0x10..0x3C left 0)
	put32(be, VTX1_OFF+VTX_FMT+0x00, 9);  // attr=GX_VA_POS
	put32(be, VTX1_OFF+VTX_FMT+0x04, 1);  // cnt=GX_POS_XYZ
	put32(be, VTX1_OFF+VTX_FMT+0x08, 4);  // type=GX_F32
	be[VTX1_OFF+VTX_FMT+0x0C]=0;          // frac (u8, no swap)
	put32(be, VTX1_OFF+VTX_FMT+0x10, 0xFF);  // GX_VA_NULL terminator
	putf(be, VTX1_OFF+VTX_POS+0x00, 1.5f); putf(be, VTX1_OFF+VTX_POS+0x04, -2.5f);
	putf(be, VTX1_OFF+VTX_POS+0x08, 3.5f);
	// EVP1 block: mWEvlpMtxNum=1, u8 count[1]={2} @0x1C, u16 idx[2] @0x20,
	//   f32 weight[2] @0x28, Mtx[jointNum=1] (f32[12]) @0x30.
	const uint32_t EVP_NUM=0x1C, EVP_IDX=0x20, EVP_W=0x28, EVP_INV=0x30;
	put32(be, EVP1_OFF+0x00, 0x45565031); // 'EVP1'
	put32(be, EVP1_OFF+0x04, EVP1_SIZE);
	put16(be, EVP1_OFF+0x08, 0x0001);     // mWEvlpMtxNum
	put32(be, EVP1_OFF+0x0C, EVP_NUM);
	put32(be, EVP1_OFF+0x10, EVP_IDX);
	put32(be, EVP1_OFF+0x14, EVP_W);
	put32(be, EVP1_OFF+0x18, EVP_INV);
	be[EVP1_OFF+EVP_NUM] = 2;             // count[0]=2 (u8, no swap)
	put16(be, EVP1_OFF+EVP_IDX+0, 0x0005); put16(be, EVP1_OFF+EVP_IDX+2, 0x0006);
	putf(be, EVP1_OFF+EVP_W+0, 0.25f); putf(be, EVP1_OFF+EVP_W+4, 0.75f);
	for (int k=0;k<12;k++) putf(be, EVP1_OFF+EVP_INV+k*4, (float)(k+1));
	// DRW1 block: mMtxNum=3, flags u8[3] @0x14, indices u16[3] @0x18
	put32(be, DRW1_OFF+0x00, 0x44525731); // 'DRW1'
	put32(be, DRW1_OFF+0x04, DRW1_SIZE);  // block size
	put16(be, DRW1_OFF+0x08, 0x0003);     // mMtxNum
	put32(be, DRW1_OFF+0x0C, 0x00000014); // mpDrawMtxFlag offset
	put32(be, DRW1_OFF+0x10, 0x00000018); // mpDrawMtxIndex offset
	be[DRW1_OFF+0x14]=1; be[DRW1_OFF+0x15]=0; be[DRW1_OFF+0x16]=1;  // flags (u8, no swap)
	put16(be, DRW1_OFF+0x18, 0x0100); put16(be, DRW1_OFF+0x1A, 0x0101);
	put16(be, DRW1_OFF+0x1C, 0x0102);     // indices (u16, swapped)
	// JNT1 block: jointNum=1, init @0x18 (stride 0x40), idx u16[1] @0x58, name=0
	const uint32_t JNT_INIT=0x18, JNT_IDX=0x58;
	put32(be, JNT1_OFF+0x00, 0x4A4E5431); // 'JNT1'
	put32(be, JNT1_OFF+0x04, JNT1_SIZE);
	put16(be, JNT1_OFF+0x08, 0x0001);     // mJointNum
	put32(be, JNT1_OFF+0x0C, JNT_INIT);
	put32(be, JNT1_OFF+0x10, JNT_IDX);
	put32(be, JNT1_OFF+0x14, 0x00000000); // mpNameTable = 0
	{
		uint32_t b = JNT1_OFF + JNT_INIT;
		put16(be, b+0x00, 0x0002);        // mKind
		be[b+0x02] = 1;                   // mScaleCompensate (u8, no swap)
		putf(be, b+0x04, 1.0f); putf(be, b+0x08, 1.0f); putf(be, b+0x0C, 1.0f); // mScale
		put16(be, b+0x10, 0x1000); put16(be, b+0x12, 0x2000); put16(be, b+0x14, 0x3000); // mRotation s16
		putf(be, b+0x18, 10.0f); putf(be, b+0x1C, 20.0f); putf(be, b+0x20, 30.0f); // mTranslate
		putf(be, b+0x24, 100.0f);         // mRadius
		putf(be, b+0x28, -1.0f); putf(be, b+0x2C, -2.0f); putf(be, b+0x30, -3.0f); // mMin
		putf(be, b+0x34,  1.0f); putf(be, b+0x38,  2.0f); putf(be, b+0x3C,  3.0f); // mMax
	}
	put16(be, JNT1_OFF+JNT_IDX, 0x0000);  // mpIndexTable[0]

	std::vector<uint8_t> out;
	BmdSwapResult r = bmd_swap_to_host(be.data(), be.size(), out);

	CK(r.ok, "swap ok");
	CK(r.block_num == 5, "block_num == 5");
	CK(r.blocks_covered == 5 && r.all_covered, "INF1+VTX1+EVP1+DRW1+JNT1 covered + all_covered");

	// VTX1: fmt-list attr/cnt/type swapped, frac u8 untouched, pos f32 swapped.
	CK(h32(out.data()+VTX1_OFF+VTX_FMT+0x00)==9, "VTX1 fmt attr");
	CK(h32(out.data()+VTX1_OFF+VTX_FMT+0x08)==4, "VTX1 fmt type");
	CK(h32(out.data()+VTX1_OFF+VTX_FMT+0x10)==0xFF, "VTX1 fmt NULL term");
	{ float px; memcpy(&px,out.data()+VTX1_OFF+VTX_POS+0x00,4); CK(px==1.5f,"VTX1 pos[0]");
	  float pz; memcpy(&pz,out.data()+VTX1_OFF+VTX_POS+0x08,4); CK(pz==3.5f,"VTX1 pos[2]"); }

	// EVP1: mWEvlpMtxNum + u16 indices + f32 weights + inv-bind matrix swapped;
	// u8 count array left intact.
	CK(h16(out.data()+EVP1_OFF+0x08)==0x0001, "EVP1 mWEvlpMtxNum");
	CK(out[EVP1_OFF+EVP_NUM]==2, "EVP1 count[0] u8 untouched");
	CK(h16(out.data()+EVP1_OFF+EVP_IDX+0)==0x0005, "EVP1 idx[0]");
	CK(h16(out.data()+EVP1_OFF+EVP_IDX+2)==0x0006, "EVP1 idx[1]");
	{ float w0; memcpy(&w0,out.data()+EVP1_OFF+EVP_W+0,4); CK(w0==0.25f,"EVP1 weight[0]"); }
	{ float m11; memcpy(&m11,out.data()+EVP1_OFF+EVP_INV+11*4,4); CK(m11==12.0f,"EVP1 inv-mtx[11]"); }

	// JNT1: jointNum + transform fields swapped, mScaleCompensate u8 untouched.
	CK(h16(out.data()+JNT1_OFF+0x08)==0x0001, "JNT1 mJointNum");
	{ uint32_t b=JNT1_OFF+JNT_INIT;
	  CK(h16(out.data()+b+0x00)==0x0002, "JNT1 mKind");
	  CK(out[b+0x02]==1, "JNT1 mScaleCompensate u8 untouched");
	  float sx; memcpy(&sx,out.data()+b+0x04,4); CK(sx==1.0f,"JNT1 mScale.x");
	  CK(h16(out.data()+b+0x10)==0x1000, "JNT1 mRotation.x (s16)");
	  float tz; memcpy(&tz,out.data()+b+0x20,4); CK(tz==30.0f,"JNT1 mTranslate.z");
	  float rad; memcpy(&rad,out.data()+b+0x24,4); CK(rad==100.0f,"JNT1 mRadius");
	  float maxz; memcpy(&maxz,out.data()+b+0x3C,4); CK(maxz==3.0f,"JNT1 mMax.z"); }
	CK(h16(out.data()+JNT1_OFF+JNT_IDX)==0x0000, "JNT1 mpIndexTable[0]");

	// Header reads host-endian after swap.
	CK(h32(out.data()+0x00)==0x4A334432, "magic host-readable");
	CK(h32(out.data()+0x08)==TOTAL,      "fileSize host-readable");
	CK(h32(out.data()+0x0C)==5,          "blockNum host-readable");
	CK(h32(out.data()+INF1_OFF)==0x494E4631, "INF1 tag host-readable");
	CK(h32(out.data()+DRW1_OFF)==0x44525731, "DRW1 tag host-readable");

	// INF1 fields.
	CK(h16(out.data()+INF1_OFF+0x08)==0x0007, "mFlags");
	CK(h32(out.data()+INF1_OFF+0x0C)==3,      "mPacketNum");
	CK(h32(out.data()+INF1_OFF+0x10)==0x11,   "mVtxNum");
	CK(h32(out.data()+INF1_OFF+0x14)==0x18,   "mpHierarchy");

	// Hierarchy entries.
	CK(h16(out.data()+INF1_OFF+0x18)==0x0010, "hier[0].type");
	CK(h16(out.data()+INF1_OFF+0x1C)==0x0011, "hier[1].type");
	CK(h16(out.data()+INF1_OFF+0x1E)==0x0001, "hier[1].value");

	// DRW1: mMtxNum + u16 index array swapped; u8 flag array left intact.
	CK(h16(out.data()+DRW1_OFF+0x08)==0x0003, "DRW1 mMtxNum");
	CK(out[DRW1_OFF+0x14]==1 && out[DRW1_OFF+0x15]==0 && out[DRW1_OFF+0x16]==1,
	   "DRW1 u8 flags untouched");
	CK(h16(out.data()+DRW1_OFF+0x18)==0x0100, "DRW1 index[0]");
	CK(h16(out.data()+DRW1_OFF+0x1C)==0x0102, "DRW1 index[2]");

	// Self-consistency: a swapped u32 equals the be32 of the original.
	CK(h32(out.data()+0x08)==be32(be.data()+0x08), "swap == be (fileSize)");

	if (fail) { printf("*** %d FAILURE(S) ***\n", fail); return 1; }
	printf("bmd_swap_test: ALL PASS\n");
	return 0;
}
