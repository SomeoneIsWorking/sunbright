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

int main() {
	int fail = 0;
	#define CK(c,m) do{ if(!(c)){ printf("FAIL: %s\n",m); fail++; } }while(0)

	// Build a synthetic big-endian BMD: 0x20 header + INF1 (0x28) + DRW1 (0x20).
	const uint32_t INF1_OFF = 0x20, INF1_SIZE = 0x28;
	const uint32_t DRW1_OFF = INF1_OFF + INF1_SIZE, DRW1_SIZE = 0x20;
	const uint32_t TOTAL = DRW1_OFF + DRW1_SIZE;
	std::vector<uint8_t> be(TOTAL, 0);
	put32(be, 0x00, 0x4A334432);          // 'J3D2'
	put32(be, 0x04, 0x626D6433);          // 'bmd3'
	put32(be, 0x08, TOTAL);               // fileSize
	put32(be, 0x0C, 2);                   // blockNum = 2
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
	// DRW1 block: mMtxNum=3, flags u8[3] @0x14, indices u16[3] @0x18
	put32(be, DRW1_OFF+0x00, 0x44525731); // 'DRW1'
	put32(be, DRW1_OFF+0x04, DRW1_SIZE);  // block size
	put16(be, DRW1_OFF+0x08, 0x0003);     // mMtxNum
	put32(be, DRW1_OFF+0x0C, 0x00000014); // mpDrawMtxFlag offset
	put32(be, DRW1_OFF+0x10, 0x00000018); // mpDrawMtxIndex offset
	be[DRW1_OFF+0x14]=1; be[DRW1_OFF+0x15]=0; be[DRW1_OFF+0x16]=1;  // flags (u8, no swap)
	put16(be, DRW1_OFF+0x18, 0x0100); put16(be, DRW1_OFF+0x1A, 0x0101);
	put16(be, DRW1_OFF+0x1C, 0x0102);     // indices (u16, swapped)

	std::vector<uint8_t> out;
	BmdSwapResult r = bmd_swap_to_host(be.data(), be.size(), out);

	CK(r.ok, "swap ok");
	CK(r.block_num == 2, "block_num == 2");
	CK(r.blocks_covered == 2 && r.all_covered, "INF1+DRW1 covered + all_covered");

	// Header reads host-endian after swap.
	CK(h32(out.data()+0x00)==0x4A334432, "magic host-readable");
	CK(h32(out.data()+0x08)==TOTAL,      "fileSize host-readable");
	CK(h32(out.data()+0x0C)==2,          "blockNum host-readable");
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
