// native_frame.cpp — the once-per-frame present point.
//
// JDrama::TVideo::waitForRetrace is the game's frame boundary: everything drawn for the
// frame has been submitted by the time it is called. That is where the GX command stream
// collected from the gather pipe is handed to aurora and the result presented.
//
// VIWaitForRetrace is deliberately NOT this point. The game spins on it from load loops,
// where presenting would be wrong; it stays a pure counter plus a scheduler drain
// (overrides/native_vi.cpp). This split is the same one the decomp runtime uses.

#include "overrides.h"

#include <aurora/aurora.h>
#include <intrinsics.h>
#include <lucent/log.h>

extern "C" void func_802fc9a4(CPUState&);   // JDrama::TVideo::waitForRetrace
extern void gxfifo_flush();

namespace {

// gpApplication (0x803E9700) field offsets from decomp/sms/include/System/Application.hpp:
// mDirector +0x04, mAppState +0x08 (u8), mCurrArea +0x0E, mNextArea +0x12.
// Reported once per frame under SBR_LUCENT_DEBUG=app so boot progress is always visible
// without rebuilding a throwaway diagnostic.
constexpr u32 GPAPPLICATION = 0x803E9700;

void report_app_state() {
    static u32 last = 0xFFFFFFFF;
    const u32 st = sb_r8(GPAPPLICATION + 0x08);
    if (st == last) return;
    last = st;
    static const char* kNames[] = {"WAIT", "DEFAULT", "BOOT", "NLOGO", "DONE",
                                   "GAMEPLAY", "MOVIE", "QUIT", "TITLE", "MENU"};
    // TGameSequence is {u8 stage, u8 scenario, u16 flags}: mCurrArea +0x0E, mNextArea +0x12.
    lucent::info("app", "mAppState -> {} ({})  curr={{{},{}}} next={{{},{}}}", st,
                 st < (sizeof(kNames) / sizeof(*kNames)) ? kNames[st] : "?",
                 sb_r8(GPAPPLICATION + 0x0E), sb_r8(GPAPPLICATION + 0x0F),
                 sb_r8(GPAPPLICATION + 0x12), sb_r8(GPAPPLICATION + 0x13));
}

void video_wait_for_retrace(CPUState& cpu) {
    report_app_state();
    // Let the game do its own frame bookkeeping first.
    func_802fc9a4(cpu);

    // Everything for this frame is in the stream now.
    gxfifo_flush();

    aurora_end_frame();
    if (!aurora_begin_frame()) {
        // A lost swapchain (resize, minimise) — the frame is discarded, not an error.
        aurora_discard_frame();
    }
}

} // namespace

SB_OVERRIDE(0x802fc9a4u, video_wait_for_retrace, "JDrama::TVideo::waitForRetrace",
            "frame boundary: hand the collected GX stream to aurora and present")
