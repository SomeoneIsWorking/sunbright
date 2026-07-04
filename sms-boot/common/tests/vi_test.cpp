// vi_test.cpp — TDD harness for the VI heartbeat seam (native/platform/vi_impl.cpp).
// Verifies the retrace counter, field alternation, pre/post retrace callback dispatch
// (with the right count), framebuffer bookkeeping, present-hook delivery, and that
// pacing-ON actually paces to ~1/60s. Pacing is disabled for the logic checks.

#include <dolphin/vi.h>
#include "vi_present.h"
#include <chrono>
#include <cstdio>
#include <thread>

static int g_fail = 0, g_checks = 0;
static void chk(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("  FAIL: %s\n", what); }
}

static u32 g_pre_count = 0, g_post_count = 0, g_pre_calls = 0;
static void pre_cb(u32 c) { g_pre_count = c; ++g_pre_calls; }
static void post_cb(u32 c) { g_post_count = c; }

static void* g_presented = (void*)1;
static int g_present_calls = 0;
static void present_hook(void* fb, void*) { g_presented = fb; ++g_present_calls; }

static void test_heartbeat() {
    VIInit();
    sb_vi_set_pacing(false);  // no sleeping for logic checks
    VISetPreRetraceCallback(pre_cb);
    VISetPostRetraceCallback(post_cb);
    sb_vi_set_present_hook(present_hook, nullptr);

    chk(VIGetRetraceCount() == 0, "retrace starts at 0");
    int field0 = VIGetNextField();

    VIWaitForRetrace();
    chk(VIGetRetraceCount() == 1, "retrace increments");
    chk(g_pre_count == 1 && g_post_count == 1, "callbacks fire with count 1");
    chk(VIGetNextField() != field0, "field alternates");

    VIWaitForRetrace();
    chk(VIGetRetraceCount() == 2, "retrace increments again");
    chk(g_post_count == 2, "post cb count 2");
    chk(VIGetNextField() == field0, "field alternates back");
    chk(g_present_calls == 2, "present hook fired per retrace");
}

static void test_fb_and_format() {
    int fbmem;
    VISetNextFrameBuffer(&fbmem);
    chk(sb_vi_current_framebuffer() == &fbmem, "framebuffer bookkeeping");
    VIWaitForRetrace();
    chk(g_presented == &fbmem, "present hook gets current fb");
    chk(VIGetTvFormat() == (u32)VI_TVMODE_NTSC_INT, "NTSC format");
    chk(VIGetDTVStatus() == 0, "no DTV");
}

static void test_pacing() {
    VIInit();
    sb_vi_set_pacing(true);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i) VIWaitForRetrace();  // ~3/60s ~= 50ms
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    chk(dt >= 30, "pacing-ON paces ~3 fields (>=30ms)");
    sb_vi_set_pacing(false);
}

int main() {
    std::printf("== VI seam unit tests ==\n");
    test_heartbeat();
    test_fb_and_format();
    test_pacing();
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
