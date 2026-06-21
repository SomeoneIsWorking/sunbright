// vi_impl.cpp — native PC implementation of the GameCube VI (video interface) seam (E5).
//
// The VI seam is the 60 Hz frame HEARTBEAT that paces the whole engine. Implements
// the 10 unresolved VI* symbols: VIInit, VIWaitForRetrace (advance the retrace
// counter, fire pre/post retrace callbacks, pace to ~60 Hz on the host clock, then
// invoke the present hook), VISetPre/PostRetraceCallback, VISetNextFrameBuffer +
// VIConfigure/VIFlush/VISetBlack (framebuffer/mode bookkeeping), VIGetRetraceCount,
// VIGetNextField, VIGetTvFormat (NTSC), VIGetDTVStatus.
//
// The ACTUAL present (host swapchain) is the integration layer's job via the
// vi_present.h hook — the seam keeps VI GameCube-free and the heartbeat logic
// (counter/field/callbacks) is unit-testable headless. NTSC 60 Hz assumed (SMS USA).

#include <dolphin/vi.h>
#include "vi_present.h"

#include <atomic>
#include <chrono>
#include <thread>

// OSGetTime from the OS seam — same lib; pace on the GC timebase for consistency.
extern "C" long long OSGetTime(void);
// Cooperative scheduler drive (os_impl.cpp): run async-resource workers (THP decode,
// DVD, audio) to idle before each retrace — the native stand-in for "main blocks on
// the VI interrupt while lower-priority worker threads run".
extern "C" void sb_sched_drain_until_idle(void);
extern "C" void sb_watchdog_kick(void);  // forward-progress heartbeat (watchdog_impl.cpp)
static const long long kTBClock = 40500000LL;       // GC timebase 40.5 MHz
static const long long kRetraceTicks = kTBClock / 60; // one NTSC field

namespace {
std::atomic<u32> g_retrace{0};
std::atomic<u32> g_field{0};
VIRetraceCallback g_pre = nullptr;
VIRetraceCallback g_post = nullptr;
void* g_fb = nullptr;
bool g_black = false;
bool g_pacing = true;
long long g_last_retrace_time = 0;
ViPresentFn g_present = nullptr;
void* g_present_user = nullptr;
ViTickFn g_tick = nullptr;
void* g_tick_user = nullptr;
}

extern "C" {

// ---- present/pacing hooks (vi_present.h) ----------------------------------
void  sb_vi_set_pacing(bool on) { g_pacing = on; }
void* sb_vi_current_framebuffer(void) { return g_fb; }
void  sb_vi_set_present_hook(ViPresentFn fn, void* user) { g_present = fn; g_present_user = user; }
void  sb_vi_set_tick_hook(ViTickFn fn, void* user) { g_tick = fn; g_tick_user = user; }

// ---- SDK VI ---------------------------------------------------------------
void VIInit(void) {
    g_retrace = 0;
    g_field = 0;
    g_fb = nullptr;
    g_black = false;
    g_last_retrace_time = OSGetTime();
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback old = g_pre; g_pre = cb; return old;
}
VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback old = g_post; g_post = cb; return old;
}

void VIWaitForRetrace(void) {
    // Run the async-resource worker threads (THP decode, DVD, audio) until the
    // system is idle, then proceed with the retrace. Cooperatively serial: only
    // one thread runs at a time, so this is where the workers get to make progress.
    sb_sched_drain_until_idle();
    sb_watchdog_kick();

    // Pace to the next ~1/60s field boundary on the host clock (the engine's
    // frame heartbeat). Tests disable pacing so they don't sleep.
    if (g_pacing) {
        long long now = OSGetTime();
        long long target = g_last_retrace_time + kRetraceTicks;
        if (target > now) {
            long long waitTicks = target - now;
            auto ns = std::chrono::nanoseconds(
                (long long)((double)waitTicks * 1e9 / (double)kTBClock));
            std::this_thread::sleep_for(ns);
        }
        g_last_retrace_time = OSGetTime();
    }

    u32 count = ++g_retrace;
    g_field ^= 1u;
    if (g_tick) g_tick(count, g_tick_user);
    if (g_pre) g_pre(count);
    if (g_post) g_post(count);
    if (g_present) g_present(g_fb, g_present_user);
}

void VIConfigure(GXRenderModeObj* /*rm*/) {}   // mode set; surface owned by integration
void VIConfigurePan(u16, u16, u16, u16) {}
void VIFlush(void) {}                           // register flush is a no-op on host
void VISetNextFrameBuffer(void* fb) { g_fb = fb; }
void VISetBlack(BOOL black) { g_black = (black != FALSE); }

u32 VIGetRetraceCount(void) { return g_retrace.load(); }
u32 VIGetNextField(void)    { return g_field.load(); }
u32 VIGetTvFormat(void)     { return (u32)VI_TVMODE_NTSC_INT; }  // SMS USA = NTSC
u32 VIGetDTVStatus(void)    { return 0; }                        // no progressive/DTV

} // extern "C"
