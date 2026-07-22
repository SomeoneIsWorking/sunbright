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
#include "../runtime/probe_server.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <intrinsics.h>
#include <lucent/log.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>

extern "C" void func_802fc9a4(CPUState&);   // JDrama::TVideo::waitForRetrace
extern void gxfifo_flush();

namespace {

// gpApplication (0x803E9700) field offsets from decomp/sms/include/System/Application.hpp:
// mDirector +0x04, mAppState +0x08 (u8), mCurrArea +0x0E, mNextArea +0x12.
// Reported once per frame under SBR_LUCENT_DEBUG=app so boot progress is always visible
// without rebuilding a throwaway diagnostic.
constexpr u32 GPAPPLICATION = 0x803E9700;

// Mario's world position, per the RE in debug_journal/2026-06-19_n7_particles_carve.md:
// SMS_GetMarioPos() is `lwz r3, -0x60B4(r13)` with r13 = 0x804141C0, i.e. 0x8040E10C holds a
// POINTER to the Mario object, and his TVec3 position is that object's first field (+0x00).
// Reported under `mario` so a scripted run can actually STEER: file-select is entered by walking
// Mario into a floating file block, and a stick script driving blind cannot tell whether he moved.
constexpr u32 GPMARIO_PTR = 0x8040E10C;

float guest_f32(u32 addr) {
    const u32 bits = sb_r32(addr);
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

void report_mario_pos() {
    static long n = 0;
    if (++n % 30 != 0) return;
    if (!sb_ram_fast(GPMARIO_PTR)) return;
    const u32 mario = sb_r32(GPMARIO_PTR);
    if (!sb_ram_fast(mario)) return;   // null before the player object exists
    lucent::debug("mario", "pos ({:.1f}, {:.1f}, {:.1f})", guest_f32(mario), guest_f32(mario + 4),
                  guest_f32(mario + 8));
}

void report_app_state() {
    static u32 last = 0xFFFFFFFF;
    static u32 lastArea = 0xFFFFFFFF;
    const u32 st = sb_r8(GPAPPLICATION + 0x08);
    // The area pair changes WITHOUT an mAppState change whenever the game asks to move to a new
    // stage: setNextStage writes mNextArea while the app stays in GAMEPLAY. Reporting only on
    // mAppState made a whole stage transition — request, load, and bounce back — invisible.
    const u32 area = (u32)sb_r8(GPAPPLICATION + 0x0E) << 24 | (u32)sb_r8(GPAPPLICATION + 0x0F) << 16 |
                     (u32)sb_r8(GPAPPLICATION + 0x12) << 8 | (u32)sb_r8(GPAPPLICATION + 0x13);
    if (st == last && area == lastArea) return;
    last = st;
    lastArea = area;
    static const char* kNames[] = {"WAIT", "DEFAULT", "BOOT", "NLOGO", "DONE",
                                   "GAMEPLAY", "MOVIE", "QUIT", "TITLE", "MENU"};
    // TGameSequence is {u8 stage, u8 scenario, u16 flags}: mCurrArea +0x0E, mNextArea +0x12.
    lucent::info("app", "mAppState -> {} ({})  curr={{{},{}}} next={{{},{}}}", st,
                 st < (sizeof(kNames) / sizeof(*kNames)) ? kNames[st] : "?",
                 sb_r8(GPAPPLICATION + 0x0E), sb_r8(GPAPPLICATION + 0x0F),
                 sb_r8(GPAPPLICATION + 0x12), sb_r8(GPAPPLICATION + 0x13));
    // mMovie (+0x18) identifies which movie a MOVIE state is trying to play.
    if (st == 6) lucent::info("app", "  mMovie = {}", sb_r32(GPAPPLICATION + 0x18));
}

// Aurora gates several of its diagnostics on a frame ordinal it obtains by calling a WEAK
// VIGetRetraceCount that the runtime is expected to provide (sms-boot does, from its frame
// seam). This runtime provided none, so the weak symbol resolved to null, aurora's counter
// read 0 forever, and every retrace-gated diagnostic — SB_DRAW_DUMP_AFTER,
// SB_DRAW_DUMP_FRAME, the SB_NDC_DRAW window — silently produced nothing. They did not report
// being unavailable; they simply never fired.
//
// Providing it here makes aurora's whole existing diagnostic toolkit work for the recomp on
// the same terms as the decomp runtime, rather than being decomp-only by accident.
extern "C" unsigned VIGetRetraceCount(void);
namespace { unsigned g_present_count = 0; }
extern "C" unsigned VIGetRetraceCount(void) { return g_present_count; }

// One NTSC field. The game asks for N retraces per frame (30fps scenes ask for 2), so pacing to
// the count IT requested is what keeps its own timing math and the wall clock agreeing.
constexpr int64_t kFieldNs = 1000000000LL * 1001 / 60000;

int64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

bool turbo() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SB_TURBO");
        v = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    return v == 1;
}

int64_t g_nextDeadlineNs = 0;

// Set from a signal handler, so it must be async-signal-safe: a volatile sig_atomic_t flag and
// nothing else. The frame loop acts on it at the frame boundary, where shutting aurora down is
// safe — a handler cannot do that itself.
volatile std::sig_atomic_t g_quit_requested = 0;

extern "C" void sb_quit_signal(int sig) { g_quit_requested = sig; }

// SIGINT (Ctrl-C) and SIGTERM (kill, and what a session manager sends at logout) must both bring
// the process down cleanly. Without handlers the default action kills it outright, leaving the
// GPU device and audio stream to be torn down by the OS.
struct QuitSignals {
    QuitSignals() {
        std::signal(SIGINT, sb_quit_signal);
        std::signal(SIGTERM, sb_quit_signal);
    }
} g_quitSignals;

void video_wait_for_retrace(CPUState& cpu) {
    ++g_present_count;
    report_app_state();
    report_mario_pos();
    // The probe's handlers run HERE, on the game thread at the frame boundary, which is the only
    // point guest memory is coherent. See probe_server.h.
    sb_probe_start();
    sb_probe_pump();

    // How far the GUEST's own retrace counter advances per rendered frame. Game code paces
    // animation off this, and the decomp runtime advances it once per NTSC field (twice per
    // frame) — so a different step here changes every time-driven thing in the game.
    {
        const u32 addr = cpu.gpr[13] - 22768;
        if (sb_ram_fast(addr)) {
            static u32 prev = 0;
            const u32 now = sb_r32(addr);
            static long n = 0;
            if (++n <= 8 || n % 200 == 0)
                lucent::debug("frame", "guest retrace counter {} (+{} since last present)",
                              now, now - prev);
            prev = now;
        }
    }
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
    //
    // The events MUST be inspected, not merely pumped: AURORA_EXIT is how closing the window
    // asks the program to stop. Discarding it left the window uncloseable — the only way out
    // was killing the process, which is not an acceptable way to quit a game.
    const AuroraEvent* event = aurora_update();
    bool exit_requested = g_quit_requested != 0;
    while (event != nullptr && event->type != AURORA_NONE) {
        if (event->type == AURORA_EXIT) exit_requested = true;
        ++event;
    }
    if (exit_requested) {
        lucent::info("frame", "{} — shutting down", g_quit_requested ? "signal received"
                                                                    : "window closed");
        aurora_shutdown();
        std::_Exit(0);
    }

    const bool wasActive = s_frameActive;
    s_frameActive = aurora_begin_frame();
    if (s_frameActive != wasActive)
        lucent::warn("frame", "aurora_begin_frame -> {}", s_frameActive);

    // PACING. Without this the recomp runs as fast as the host allows (measured ~157 fps against
    // the oracle's 30) — every animation, timer and physics step driven off the retrace count
    // runs at whatever speed the machine happens to manage, which is not the game.
    //
    // Pace to the number of retraces the GAME asked for this frame, taken from its own counter
    // (the same counter VIWaitForRetrace advances), not to a fixed 60Hz: a 30fps scene requests
    // two fields per frame and must be paced as two. Deadline-based rather than sleep-per-frame,
    // so a frame that overruns is absorbed by the next instead of compounding drift. This mirrors
    // sms-boot/runtime/frame_seam.cpp; SB_TURBO=1 disables it in both runtimes.
    if (!turbo()) {
        const u32 addr = cpu.gpr[13] - 22768;
        static u32 s_prevRetrace = 0;
        unsigned retraces = 1;
        if (sb_ram_fast(addr)) {
            const u32 now = sb_r32(addr);
            const u32 delta = now - s_prevRetrace;
            s_prevRetrace = now;
            // A load hitch can advance the counter arbitrarily; clamp so one hitch cannot
            // translate into a multi-second sleep.
            if (delta >= 1 && delta <= 8) retraces = delta;
        }
        if (g_nextDeadlineNs == 0) g_nextDeadlineNs = now_ns();
        g_nextDeadlineNs += (int64_t)retraces * kFieldNs;
        const int64_t now = now_ns();
        if (now < g_nextDeadlineNs) {
            const int64_t d = g_nextDeadlineNs - now;
            timespec ts{(time_t)(d / 1000000000LL), (long)(d % 1000000000LL)};
            nanosleep(&ts, nullptr);
        } else if (now - g_nextDeadlineNs > 4 * kFieldNs) {
            // Fell far behind (load hitch): resynchronize instead of sprinting to catch up.
            g_nextDeadlineNs = now;
        }
    }
}

} // namespace

SB_OVERRIDE(0x802fc9a4u, video_wait_for_retrace, "JDrama::TVideo::waitForRetrace",
            "frame boundary: hand the collected GX stream to aurora and present")
