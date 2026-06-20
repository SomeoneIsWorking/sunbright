// ar_dsp_test.cpp — TDD harness for the inert ARAM/DSP seam (ar_dsp_impl.cpp).
// Verifies the one stateful piece (the ARAM bump allocator: 32-byte-aligned,
// monotonic, exhaustion-clamped) + the inert contracts (DSP no-mail, ARQ instant
// callback). The game-facing behavior is "consistent ARAM offsets + instant DMA".

#include <dolphin/ar.h>
#include <dolphin/dsp.h>
#include <cstdio>

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
    g_cb_fired = 0;
    ARQPostRequest(&req, 0, 0, 0, 0, 0, 256, arq_cb);
    chk(g_cb_fired == 1, "ARQ fires completion callback (instant DMA)");
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
