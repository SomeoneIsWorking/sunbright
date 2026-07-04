// ral_swap_test.cpp — verify-first unit test for the native BE->host scene.ral swap
// (reference/sms/include/Enemy/GraphSwap.h, sb_ral_swap_to_host).
//
// Builds a synthetic BIG-ENDIAN rail-graph blob (2 GraphDescs + their TRailNode
// arrays + terminator), runs the shipping swapper, then reads each field back AT THE
// SAME OFFSET Enemy/graph.cpp reads it (plain struct access) and asserts it equals the
// hand-derived host value. Also checks the terminator stops the walk, bounds-clamping,
// and idempotency (a repeat swap of the same buffer is a no-op).
//
// No GPU / no ROM / pure byte logic. The swapper IS the shipping function.

#include <Enemy/GraphSwap.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); g_fail = 1; } \
	else         { std::fprintf(stderr, "ok:   %s\n", msg); } \
} while (0)

static void put_be16(uint8_t* p, uint32_t off, uint16_t v) {
	p[off] = (uint8_t)(v >> 8); p[off + 1] = (uint8_t)v;
}
static void put_be32(uint8_t* p, uint32_t off, uint32_t v) {
	p[off] = (uint8_t)(v >> 24); p[off + 1] = (uint8_t)(v >> 16);
	p[off + 2] = (uint8_t)(v >> 8); p[off + 3] = (uint8_t)v;
}
static void put_be_f32(uint8_t* p, uint32_t off, float f) {
	uint32_t u; std::memcpy(&u, &f, 4); put_be32(p, off, u);
}
static int16_t  host_s16(const uint8_t* p, uint32_t off) {
	int16_t v; std::memcpy(&v, p + off, 2); return v;
}
static uint16_t host_u16(const uint8_t* p, uint32_t off) {
	uint16_t v; std::memcpy(&v, p + off, 2); return v;
}
static int32_t  host_s32(const uint8_t* p, uint32_t off) {
	int32_t v; std::memcpy(&v, p + off, 4); return v;
}
static uint32_t host_u32(const uint8_t* p, uint32_t off) {
	uint32_t v; std::memcpy(&v, p + off, 4); return v;
}
static float    host_f32(const uint8_t* p, uint32_t off) {
	float v; std::memcpy(&v, p + off, 4); return v;
}

// Write a TRailNode with field values derived from a seed, big-endian.
static void put_be_rail(uint8_t* p, uint32_t off, int seed) {
	put_be16(p, off + 0x00, (uint16_t)(100 + seed));      // mPosition.x
	put_be16(p, off + 0x02, (uint16_t)(200 + seed));      // mPosition.y
	put_be16(p, off + 0x04, (uint16_t)(300 + seed));      // mPosition.z
	put_be16(p, off + 0x06, (uint16_t)(2));               // mConnectionNum
	put_be32(p, off + 0x08, 0x000000B0u + seed);          // mFlags
	put_be16(p, off + 0x0C, (uint16_t)(0x0100 + seed));   // mPitch
	put_be16(p, off + 0x0E, (uint16_t)(0x0200 + seed));   // mYaw
	put_be16(p, off + 0x10, (uint16_t)(0x0300 + seed));   // mRoll
	put_be16(p, off + 0x12, (uint16_t)(0x0400 + seed));   // mSpeed
	for (int j = 0; j < 8; ++j)
		put_be16(p, off + 0x14 + j * 2, (uint16_t)(10 * seed + j)); // mConnections
	for (int j = 0; j < 8; ++j)
		put_be_f32(p, off + 0x24 + j * 4, (float)(seed) + 0.25f * j); // mPeriods
}

int main() {
	using smsport::ral_detail::kRailNodeSize;
	const uint32_t DESC = 0xC;
	const uint32_t RAIL0 = 0x24;             // 3 descs (0,0xC,0x18=terminator)
	const uint32_t RAIL1 = RAIL0 + 2 * kRailNodeSize; // after 2 nodes
	const uint32_t SIZE  = RAIL1 + 1 * kRailNodeSize + 16;

	uint8_t buf[1024];
	std::memset(buf, 0, sizeof(buf));

	// GraphDesc[0]: 2 nodes at RAIL0
	put_be32(buf, 0x00, 2);
	put_be32(buf, 0x04, 0xCAFE);
	put_be32(buf, 0x08, RAIL0);
	// GraphDesc[1]: 1 node at RAIL1
	put_be32(buf, DESC + 0x00, 1);
	put_be32(buf, DESC + 0x04, 0xBEEF);
	put_be32(buf, DESC + 0x08, RAIL1);
	// GraphDesc[2]: terminator (mNodeNum = 0) — already zero.

	put_be_rail(buf, RAIL0 + 0 * kRailNodeSize, 1);
	put_be_rail(buf, RAIL0 + 1 * kRailNodeSize, 2);
	put_be_rail(buf, RAIL1 + 0 * kRailNodeSize, 3);

	smsport::sb_ral_swap_to_host(buf, SIZE);

	// GraphDesc fields (graph.cpp: unk0[i].mNodeNum / mRailNodesOffset, getNodeName).
	CHECK(host_s32(buf, 0x00) == 2, "desc0 mNodeNum");
	CHECK(host_u32(buf, 0x04) == 0xCAFE, "desc0 mNameOffset");
	CHECK(host_u32(buf, 0x08) == RAIL0, "desc0 mRailNodesOffset");
	CHECK(host_s32(buf, DESC + 0x00) == 1, "desc1 mNodeNum");
	CHECK(host_u32(buf, DESC + 0x08) == RAIL1, "desc1 mRailNodesOffset");
	CHECK(host_s32(buf, 0x18) == 0, "terminator mNodeNum stays 0");

	// TRailNode fields for node seed=1 (RAIL0) — the values graph.cpp reads.
	uint32_t n = RAIL0;
	CHECK(host_s16(buf, n + 0x00) == 101, "node0 mPosition.x");
	CHECK(host_s16(buf, n + 0x04) == 301, "node0 mPosition.z");
	CHECK(host_s16(buf, n + 0x06) == 2, "node0 mConnectionNum");
	CHECK(host_u32(buf, n + 0x08) == 0x000000B1u, "node0 mFlags (was BE 0xB1000000)");
	CHECK(host_u16(buf, n + 0x10) == 0x0301, "node0 mRoll");
	CHECK(host_s16(buf, n + 0x12) == 0x0401, "node0 mSpeed");
	CHECK(host_u16(buf, n + 0x14 + 0) == 10, "node0 mConnections[0]");
	CHECK(host_u16(buf, n + 0x14 + 14) == 17, "node0 mConnections[7]");
	CHECK(host_f32(buf, n + 0x24 + 0) == 1.0f, "node0 mPeriods[0]");
	CHECK(host_f32(buf, n + 0x24 + 4) == 1.25f, "node0 mPeriods[1]");

	// node seed=3 (RAIL1, the single node of graph 1).
	CHECK(host_s16(buf, RAIL1 + 0x00) == 103, "graph1 node mPosition.x");
	CHECK(host_u32(buf, RAIL1 + 0x08) == 0x000000B3u, "graph1 node mFlags");

	// Idempotency: a second swap of the same buffer is a no-op (now detected BY CONTENT —
	// the host-order node count is already a sane small value).
	smsport::sb_ral_swap_to_host(buf, SIZE);
	CHECK(host_s32(buf, 0x00) == 2, "desc0 mNodeNum unchanged after repeat swap");
	CHECK(host_u32(buf, RAIL0 + 0x08) == 0x000000B1u, "node0 mFlags unchanged after repeat");

	// The content predicate directly (self-describing idempotency).
	{
		uint8_t be[16]; std::memset(be, 0, sizeof(be)); put_be32(be, 0, 18); // 00 00 00 12
		CHECK(!smsport::ral_already_swapped(be), "BE buffer (nodeNum 18) -> needs swap");
		uint8_t host[16]; std::memset(host, 0, sizeof(host));
		host[0] = 18;                                                        // 12 00 00 00 = LE 18
		CHECK(smsport::ral_already_swapped(host), "host-order buffer (nodeNum 18) -> already swapped");
		uint8_t empty[16]; std::memset(empty, 0, sizeof(empty));
		CHECK(!smsport::ral_already_swapped(empty), "empty/terminator buffer -> not 'already swapped'");
	}

	// REGRESSION (the real bug): a pointer-keyed cache wrongly reported "already swapped"
	// when the JKR heap reused a freed scene.ral's address for the NEXT stage's buffer, so
	// the fresh big-endian data was read raw -> ~2.9 GB `new TGraphNode[]` -> SolidHeap OOM.
	// Overwrite the SAME address with fresh BE content and confirm it re-swaps.
	std::memset(buf, 0, sizeof(buf));
	put_be32(buf, 0x00, 5);        // desc0: 5 nodes
	put_be32(buf, 0x08, RAIL0);
	put_be32(buf, DESC, 0);        // terminator
	for (int i = 0; i < 5; ++i) put_be_rail(buf, RAIL0 + i * kRailNodeSize, i + 1);
	smsport::sb_ral_swap_to_host(buf, SIZE);
	CHECK(host_s32(buf, 0x00) == 5, "pointer-reuse: fresh BE buffer at same addr re-swaps (nodeNum 5)");
	CHECK(host_u32(buf, RAIL0 + 0x08) == 0x000000B1u, "pointer-reuse: node0 mFlags swapped");

	std::fprintf(stderr, "%s\n", g_fail ? "RAL SWAP TEST FAILED" : "ral swap test ok");
	return g_fail;
}
