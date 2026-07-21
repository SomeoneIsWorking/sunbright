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

#include <chrono>

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

    // Everything for this frame is in the stream now. Only hand it over if a frame is
    // actually open; otherwise it would be replayed into a frame that will be discarded.
    gxfifo_flush();

    // Present rate, so "is it slow?" is measured rather than guessed.
    {
        using clock = std::chrono::steady_clock;
        static auto t0 = clock::now();
        static long frames = 0;
        if (++frames % 30 == 0) {
            const auto now = clock::now();
            const double s = std::chrono::duration<double>(now - t0).count();
            lucent::debug("frame", "{} presents, {:.1f}/s over the last 30", frames, 30.0 / s);
            t0 = now;
        }
    }

    // Frame bookkeeping, per aurora's contract:
    //  - end_frame must NOT run if the matching begin_frame returned false
    //  - a frame begun but not presentable must be DISCARDED, or the fifo grows unbounded
    //  - aurora_update() is the event pump; without it the window/swapchain state never
    //    advances
    //
    // Getting this wrong made the per-frame staging buffer accumulate across frames instead
    // of resetting: ~291 KB of stream per frame reached aurora's 48 MB limit after roughly
    // 170 frames and aborted with "mapped ByteBuffer overflow".
    static bool s_frameActive = true;   // main() opened the first frame

    if (s_frameActive) {
        aurora_end_frame();
    } else {
        aurora_discard_frame();
    }

    // Once per frame: aurora_update returns the frame's event ARRAY, it is not a
    // pop-one-at-a-time queue. Calling it in a loop never terminates.
    aurora_update();

    const bool wasActive = s_frameActive;
    s_frameActive = aurora_begin_frame();
    if (s_frameActive != wasActive)
        lucent::warn("frame", "aurora_begin_frame -> {}", s_frameActive);
}

} // namespace

SB_OVERRIDE(0x802fc9a4u, video_wait_for_retrace, "JDrama::TVideo::waitForRetrace",
            "frame boundary: hand the collected GX stream to aurora and present")
