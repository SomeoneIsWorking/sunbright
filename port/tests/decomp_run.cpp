// Runtime test: the REAL engine async-decompress worker (JKRDecomp, a JKRThread
// subclass in core) running natively on the cooperative scheduler (os_thread.cpp).
//
// This validates two things at once:
//   1. The native threading (runtime step 2) drives a REAL engine worker — not
//      the synthetic EchoThread. JKRDecomp::create spawns the decomp thread;
//      orderSync submits a command over a BLOCKING message queue and blocks on a
//      reply queue until the worker decodes it. The whole producer/worker/reply
//      handoff is the engine's own code.
//   2. The Yaz0 (.szs) decoder is endianness-correct. decodeSZS used to read the
//      decompressed-size header native-endian (a little-endian corruption bug);
//      this test decodes hand-built Yaz0 streams and checks the bytes, so the bug
//      fails the test and the READU32_BE fix passes it.
#include <JSystem/JKernel/JKRDecomp.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <dolphin/os.h>

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

extern "C" void sb_heap_bringup();

static int g_fail = 0;
#define CHECK(cond, msg) do { \
	if (cond) { std::printf("[decomp_run]   ok: %s\n", msg); } \
	else      { std::printf("[decomp_run] FAIL: %s\n", msg); g_fail = 1; } \
} while (0)

// Build a Yaz0 header: "Yaz0", big-endian decompressed size, 8 reserved bytes.
static void push_header(std::vector<u8>& b, u32 decompSize) {
	b.push_back('Y'); b.push_back('a'); b.push_back('z'); b.push_back('0');
	b.push_back((decompSize >> 24) & 0xFF);
	b.push_back((decompSize >> 16) & 0xFF);
	b.push_back((decompSize >> 8) & 0xFF);
	b.push_back(decompSize & 0xFF);
	for (int i = 0; i < 8; i++) b.push_back(0);
}

// Literal-only Yaz0 stream of `data` (each control byte flags 8 literals).
static std::vector<u8> yaz0_literals(const char* data, u32 n) {
	std::vector<u8> b;
	push_header(b, n);
	for (u32 i = 0; i < n; ) {
		u32 grp = (n - i < 8) ? (n - i) : 8;
		u8 ctrl = 0;
		for (u32 k = 0; k < grp; k++) ctrl |= (0x80 >> k);  // top `grp` bits = literals
		b.push_back(ctrl);
		for (u32 k = 0; k < grp; k++) b.push_back((u8)data[i + k]);
		i += grp;
	}
	return b;
}

int main() {
	std::setbuf(stdout, nullptr);
	sb_heap_bringup();

	// Spawn the real decomp worker (priority 10: higher than the bootstrap main
	// at 16, so it runs its prologue — OSInitMessageQueue — before we submit).
	JKRDecomp* dec = JKRDecomp::create(10);
	CHECK(dec != nullptr, "JKRDecomp::create spawned the engine decomp worker thread");

	// Case 1 — literals. Catches the decompressed-size endianness bug directly:
	// a byteswapped size makes the decode loop end at the wrong byte.
	{
		const char* expected = "Hello, World!";
		u32 n = (u32)std::strlen(expected);
		std::vector<u8> src = yaz0_literals(expected, n);
		std::vector<u8> dst(n + 8, 0xCD);
		bool ok = JKRDecomp::orderSync(src.data(), dst.data(), (u32)src.size(), 0);
		CHECK(ok, "orderSync (literal stream) completed on the worker thread");
		CHECK(std::memcmp(dst.data(), expected, n) == 0,
		      "literal Yaz0 decoded correctly across the thread boundary");
		CHECK(dst[n] == 0xCD, "decode wrote exactly decompSize bytes (no overrun)");
	}

	// Case 2 — a back-reference (exercises the copy path + loop terminator):
	// "ABCABC" = literals A,B,C then copy 3 bytes from distance 3.
	{
		std::vector<u8> src;
		push_header(src, 6);
		src.push_back(0xE0);                              // 111 literals, then a copy
		src.push_back('A'); src.push_back('B'); src.push_back('C');
		src.push_back(0x10); src.push_back(0x02);         // r31=1 -> len 3; dist 3
		std::vector<u8> dst(6 + 4, 0xCD);
		bool ok = JKRDecomp::orderSync(src.data(), dst.data(), (u32)src.size(), 0);
		CHECK(ok, "orderSync (back-reference stream) completed");
		CHECK(std::memcmp(dst.data(), "ABCABC", 6) == 0,
		      "back-reference Yaz0 decoded correctly");
		CHECK(dst[6] == 0xCD, "back-ref decode wrote exactly 6 bytes (no overrun)");
	}

	if (g_fail) { std::printf("[decomp_run] RESULT: FAIL\n"); return 1; }
	std::printf("[decomp_run] RESULT: PASS — real engine decomp worker runs + Yaz0 LE-correct\n");
	return 0;
}
