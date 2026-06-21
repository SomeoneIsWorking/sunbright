// ar_dsp_test.cpp — TDD harness for the ARAM/DSP seam (ar_dsp_impl.cpp).
// Verifies the ARAM bump allocator (32-byte-aligned, monotonic, exhaustion-clamped),
// the DSP no-mail contract, and — the part that is NO LONGER inert — that the ARQ DMA
// actually moves bytes through host-backed ARAM both directions (the 2D/UI archive
// load path depends on this; the old no-copy stub left ARAM empty -> unmounted
// "guide" volume). Round-trip: MRAM->ARAM then ARAM->MRAM must reproduce the bytes.

#include <dolphin/ar.h>
#include <dolphin/dsp.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

static int g_fail = 0, g_checks = 0;
static void chk(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("  FAIL: %s\n", what); }
}

static void test_aram() {
    ARInit(nullptr, 0);
    chk(ARGetSize() == (16u << 20), "ARAM size 16MB");
    u32 a = ARAlloc(100);
    u32 b = ARAlloc(100);
    chk(b == a + 128, "ARAlloc 32-aligned bump (100 -> 128)");
    chk((a % 32) == 0 && (b % 32) == 0, "ARAM addrs 32-aligned");
    chk(b > a, "ARAM addrs monotonic");
}

static int g_cb_fired = 0;
static void arq_cb(ARQRequestRef) { ++g_cb_fired; }
static void test_arq() {
    ARQInit();
    ARQRequest req;

    // 256 bytes of known data, 32-byte aligned (ARQ alignment requirement).
    alignas(32) u8 srcbuf[256], dstbuf[256];
    for (int i = 0; i < 256; ++i) { srcbuf[i] = (u8)(i * 7 + 3); dstbuf[i] = 0; }

    u32 aramOff = ARAlloc(256);

    // MRAM -> ARAM (type ARAM_DIR_MRAM_TO_ARAM): source=host ptr, dest=ARAM offset.
    g_cb_fired = 0;
    ARQPostRequest(&req, 0, ARAM_DIR_MRAM_TO_ARAM, 0, (ARMemAddr)(uintptr_t)srcbuf,
                   (ARMemAddr)aramOff, 256, arq_cb);
    chk(g_cb_fired == 1, "ARQ fires completion callback (MRAM->ARAM)");

    // ARAM -> MRAM (type ARAM_DIR_ARAM_TO_MRAM): source=ARAM offset, dest=host ptr.
    g_cb_fired = 0;
    ARQPostRequest(&req, 0, ARAM_DIR_ARAM_TO_MRAM, 0, (ARMemAddr)aramOff,
                   (ARMemAddr)(uintptr_t)dstbuf, 256, arq_cb);
    chk(g_cb_fired == 1, "ARQ fires completion callback (ARAM->MRAM)");

    chk(std::memcmp(srcbuf, dstbuf, 256) == 0,
        "ARQ DMA round-trips bytes through host-backed ARAM");
}

static void test_dsp() {
    chk(DSPCheckMailFromDSP() == 0, "DSP no mail pending");
    chk(DSPReadMailFromDSP() == 0, "DSP read mail = 0");
}

int main() {
    std::printf("== AR/DSP seam unit tests ==\n");
    test_aram();
    test_arq();
    test_dsp();
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
