// jpa_swap_test.cpp — verify-first unit test for the native BE->host .jpa swap
// (reference/sms/include/JSystem/JParticle/JPASwap.h, sb_jpa_swap_to_host).
//
// Builds a synthetic BIG-ENDIAN JPA1 blob (header + one of each handled block),
// runs the shipping swapper, then reads each field back AT THE SAME OFFSET the
// decomp loader reads it and asserts it equals the hand-derived host value. Also
// checks idempotency, bad-magic no-op, and that BEM1/FLD1 fields the loaders read
// via the auto-swapping JSUInputStream readers are LEFT big-endian (no double-swap).
//
// No GPU / no ROM / pure byte logic. The swapper IS the shipping function.

#include <JSystem/JParticle/JPASwap.h>
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
// --- host readers (what the loader sees after the swap) ----------------------
static uint16_t host_u16(const uint8_t* p, uint32_t off) {
	uint16_t v; std::memcpy(&v, p + off, 2); return v;
}
static uint32_t host_u32(const uint8_t* p, uint32_t off) {
	uint32_t v; std::memcpy(&v, p + off, 4); return v;
}
static float host_f32(const uint8_t* p, uint32_t off) {
	float v; std::memcpy(&v, p + off, 4); return v;
}

static const uint32_t kBlockSize = 0x100;
static const uint32_t kHdr       = 0x20;

// block bases
enum { BSP1 = 0, ESP1, SSP1, ETX1, KFA1, BEM1, FLD1, NBLK };
static uint32_t blkBase(int i) { return kHdr + (uint32_t)i * kBlockSize; }

static std::vector<uint8_t> build_blob() {
	std::vector<uint8_t> v(kHdr + NBLK * kBlockSize, 0);
	uint8_t* b = v.data();

	// header: 'JEFF' 'jpa1' unk8 blockCount
	put_be32(b, 0x00, smsport::jpa_detail::fourcc('J', 'E', 'F', 'F'));
	put_be32(b, 0x04, smsport::jpa_detail::fourcc('j', 'p', 'a', '1'));
	put_be32(b, 0x08, 0xCAFEBABE);
	put_be32(b, 0x0C, NBLK);

	const char* tags[NBLK] = { "BSP1", "ESP1", "SSP1", "ETX1",
	                           "KFA1", "BEM1", "FLD1" };
	for (int i = 0; i < NBLK; ++i) {
		uint32_t base = blkBase(i);
		put_be32(b, base + 0, smsport::jpa_detail::fourcc(
		                          tags[i][0], tags[i][1], tags[i][2], tags[i][3]));
		put_be32(b, base + 4, kBlockSize);  // nextOffset == block size
	}

	// --- BSP1 fields (offsets relative to block base) ---
	uint32_t bsp = blkBase(BSP1);
	put_be16(b, bsp + 0x12, 0x00AA);          // mTextureIndices src offset
	put_be16(b, bsp + 0x14, 0x00C0);          // prm key offset
	put_be16(b, bsp + 0x16, 0x00E0);          // env key offset
	put_be_f32(b, bsp + 0x18, 12.5f);         // mBaseSizeY
	put_be_f32(b, bsp + 0x1C, -3.25f);        // mBaseSizeX
	put_be16(b, bsp + 0x20, 0x1234);          // mLoopOffset
	put_be16(b, bsp + 0x5C, 0x0040);          // mColorRegAnmMaxFrm
	put_be16(b, bsp + 0x80, 0x0102);          // mTexStaticTransX
	put_be16(b, bsp + 0x94, 0x0506);          // mTexScrollRotate
	b[bsp + 0x62] = 2;                        // prm key count (unk85)
	b[bsp + 0x63] = 1;                        // env key count (unk86)
	put_be16(b, bsp + 0xC0 + 0 * 6, 0x1111);  // prm key[0].unk0
	put_be16(b, bsp + 0xC0 + 1 * 6, 0x2222);  // prm key[1].unk0
	put_be16(b, bsp + 0xE0 + 0 * 6, 0x3333);  // env key[0].unk0
	// a GXColor that must NOT move (bytes): write 4 distinct bytes at 0x64
	b[bsp + 0x64] = 0xDE; b[bsp + 0x65] = 0xAD;
	b[bsp + 0x66] = 0xBE; b[bsp + 0x67] = 0xEF;

	// --- ESP1 ---
	uint32_t esp = blkBase(ESP1);
	put_be16(b, esp + 0x14, 0x0A0B);
	put_be16(b, esp + 0x62, 0x0C0D);

	// --- SSP1 ---
	uint32_t ssp = blkBase(SSP1);
	put_be16(b, ssp + 0x14, 0x4455);
	put_be_f32(b, ssp + 0x28, 7.0f);
	put_be_f32(b, ssp + 0x50, -1.5f);

	// --- ETX1 ---
	uint32_t etx = blkBase(ETX1);
	put_be16(b, etx + 0x12, 0x6677);
	put_be16(b, etx + 0x1C, 0x8899);

	// --- KFA1: frameNum=2 -> 8 floats at +0x20 ---
	uint32_t kfa = blkBase(KFA1);
	b[kfa + 0x10] = 2;
	for (int i = 0; i < 8; ++i)
		put_be_f32(b, kfa + 0x20 + i * 4, (float)(i + 1) * 0.5f);

	// --- BEM1: raw f32/s16 swapped; readS16 field at 0x34 must stay BE ---
	uint32_t bem = blkBase(BEM1);
	put_be_f32(b, bem + 0x0C, 2.0f);          // mScale.x (raw)
	put_be_f32(b, bem + 0x30, 9.0f);          // mChildSpawnRate (raw)
	put_be32(b, bem + 0x6C, 0x000000FF);      // mEmitFlags (raw u32)
	put_be16(b, bem + 0x24, 0x0010);          // mRot.x (raw s16)
	put_be16(b, bem + 0x34, 0x1357);          // readS16 field -> must NOT swap

	// --- FLD1: raw f32 at misaligned 0x13; readS16 field at 0x43 stays BE ---
	uint32_t fld = blkBase(FLD1);
	put_be_f32(b, fld + 0x13, 4.0f);          // unk10 (raw f32)
	put_be_f32(b, fld + 0x3F, -8.0f);         // unk38 (raw f32, last of 12)
	put_be16(b, fld + 0x43, 0x2468);          // readS16 fade -> must NOT swap

	return v;
}

static void test_swap() {
	std::vector<uint8_t> v = build_blob();
	uint8_t* b = v.data();
	sb_jpa_swap_to_host(b);

	// header now host-readable
	CHECK(host_u32(b, 0x00) == smsport::jpa_detail::fourcc('J', 'E', 'F', 'F'),
	      "header magic 'JEFF' host-readable");
	CHECK(host_u32(b, 0x04) == smsport::jpa_detail::fourcc('j', 'p', 'a', '1'),
	      "header magic 'jpa1' host-readable");
	CHECK(host_u32(b, 0x08) == 0xCAFEBABE, "header unk8 swapped");
	CHECK(host_u32(b, 0x0C) == NBLK, "header block count swapped");

	// every block header host-readable
	for (int i = 0; i < NBLK; ++i)
		CHECK(host_u32(b, blkBase(i) + 4) == kBlockSize,
		      "block nextOffset/size swapped");

	// BSP1
	uint32_t bsp = blkBase(BSP1);
	CHECK(host_u16(b, bsp + 0x12) == 0x00AA, "BSP1 texIdx offset");
	CHECK(host_u16(b, bsp + 0x14) == 0x00C0, "BSP1 prm key offset");
	CHECK(host_u16(b, bsp + 0x16) == 0x00E0, "BSP1 env key offset");
	CHECK(host_f32(b, bsp + 0x18) == 12.5f, "BSP1 mBaseSizeY");
	CHECK(host_f32(b, bsp + 0x1C) == -3.25f, "BSP1 mBaseSizeX");
	CHECK(host_u16(b, bsp + 0x20) == 0x1234, "BSP1 mLoopOffset");
	CHECK(host_u16(b, bsp + 0x5C) == 0x0040, "BSP1 mColorRegAnmMaxFrm");
	CHECK(host_u16(b, bsp + 0x80) == 0x0102, "BSP1 mTexStaticTransX");
	CHECK(host_u16(b, bsp + 0x94) == 0x0506, "BSP1 mTexScrollRotate");
	CHECK(host_u16(b, bsp + 0xC0 + 0 * 6) == 0x1111, "BSP1 prm key[0]");
	CHECK(host_u16(b, bsp + 0xC0 + 1 * 6) == 0x2222, "BSP1 prm key[1]");
	CHECK(host_u16(b, bsp + 0xE0 + 0 * 6) == 0x3333, "BSP1 env key[0]");
	CHECK(b[bsp + 0x64] == 0xDE && b[bsp + 0x65] == 0xAD &&
	          b[bsp + 0x66] == 0xBE && b[bsp + 0x67] == 0xEF,
	      "BSP1 GXColor bytes unchanged (not swapped)");

	// ESP1 / SSP1 / ETX1
	CHECK(host_u16(b, blkBase(ESP1) + 0x14) == 0x0A0B, "ESP1 @0x14");
	CHECK(host_u16(b, blkBase(ESP1) + 0x62) == 0x0C0D, "ESP1 @0x62");
	CHECK(host_u16(b, blkBase(SSP1) + 0x14) == 0x4455, "SSP1 @0x14");
	CHECK(host_f32(b, blkBase(SSP1) + 0x28) == 7.0f, "SSP1 @0x28 f32");
	CHECK(host_f32(b, blkBase(SSP1) + 0x50) == -1.5f, "SSP1 @0x50 f32");
	CHECK(host_u16(b, blkBase(ETX1) + 0x12) == 0x6677, "ETX1 @0x12");
	CHECK(host_u16(b, blkBase(ETX1) + 0x1C) == 0x8899, "ETX1 @0x1C");

	// KFA1
	uint32_t kfa = blkBase(KFA1);
	bool kfaOk = true;
	for (int i = 0; i < 8; ++i)
		if (host_f32(b, kfa + 0x20 + i * 4) != (float)(i + 1) * 0.5f) kfaOk = false;
	CHECK(kfaOk, "KFA1 8 keyframe floats swapped");

	// BEM1
	uint32_t bem = blkBase(BEM1);
	CHECK(host_f32(b, bem + 0x0C) == 2.0f, "BEM1 mScale.x (raw f32)");
	CHECK(host_f32(b, bem + 0x30) == 9.0f, "BEM1 mChildSpawnRate (raw f32)");
	CHECK(host_u32(b, bem + 0x6C) == 0x000000FF, "BEM1 mEmitFlags (raw u32)");
	CHECK(host_u16(b, bem + 0x24) == 0x0010, "BEM1 mRot.x (raw s16)");
	// the readS16 field must remain big-endian (0x1357 BE -> host 0x5713)
	CHECK(host_u16(b, bem + 0x34) == 0x5713,
	      "BEM1 @0x34 readS16 field LEFT big-endian (no double-swap)");

	// FLD1
	uint32_t fld = blkBase(FLD1);
	CHECK(host_f32(b, fld + 0x13) == 4.0f, "FLD1 unk10 (raw f32, misaligned)");
	CHECK(host_f32(b, fld + 0x3F) == -8.0f, "FLD1 unk38 (raw f32)");
	CHECK(host_u16(b, fld + 0x43) == 0x6824,
	      "FLD1 @0x43 readS16 fade LEFT big-endian (no double-swap)");
}

static void test_idempotent() {
	std::vector<uint8_t> v = build_blob();
	uint8_t* b = v.data();
	sb_jpa_swap_to_host(b);
	std::vector<uint8_t> once = v;        // snapshot after one swap
	sb_jpa_swap_to_host(b);               // second call must be a no-op
	CHECK(std::memcmp(b, once.data(), v.size()) == 0,
	      "second swap is a no-op (idempotent on host-endian magic)");
}

static void test_bad_magic() {
	std::vector<uint8_t> v(0x40, 0);
	uint8_t* b = v.data();
	put_be32(b, 0x00, 0x12345678);        // not 'JEFF'
	put_be32(b, 0x04, 0x9ABCDEF0);
	std::vector<uint8_t> before = v;
	sb_jpa_swap_to_host(b);               // must leave it untouched
	CHECK(std::memcmp(b, before.data(), v.size()) == 0,
	      "unrecognised magic left untouched (caller fails fast)");
}

int main() {
	test_swap();
	test_idempotent();
	test_bad_magic();
	std::fprintf(stderr, g_fail ? "\nFAILED\n" : "\nPASSED\n");
	return g_fail;
}
