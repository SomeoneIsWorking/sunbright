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
#include "../runtime/screen_effects.h"
#include "../runtime/native_render.h"
#include "../runtime/scene.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <intrinsics.h>
#include <lucent/log.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

extern "C" void func_802fc9a4(CPUState&);   // JDrama::TVideo::waitForRetrace
// Weak: resolves to a no-op until the audio subsystem provides it (see docs/audio_recomp_plan.md).
extern "C" __attribute__((weak)) void sbr_audio_frame();
extern "C" __attribute__((weak)) void sbr_audio_frame() {}

extern void gxfifo_build();
extern void gxfifo_send_last();
extern void gxfifo_send(const std::vector<u8>&);

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

// Sleep until `fields` more NTSC fields have elapsed. Deadline-accumulating rather than
// sleep-per-call, so a frame that overruns is absorbed by the next instead of compounding drift.
void pace_fields(unsigned fields) {
    if (turbo() || fields == 0) return;
    if (g_nextDeadlineNs == 0) g_nextDeadlineNs = now_ns();
    g_nextDeadlineNs += (int64_t)fields * kFieldNs;
    const int64_t now = now_ns();
    if (now < g_nextDeadlineNs) {
        const int64_t d = g_nextDeadlineNs - now;
        timespec ts{(time_t)(d / 1000000000LL), (long)(d % 1000000000LL)};
        nanosleep(&ts, nullptr);
    } else if (now - g_nextDeadlineNs > 4 * kFieldNs) {
        g_nextDeadlineNs = now;   // fell far behind (load hitch): resync, don't sprint to catch up
    }
}

// Present whatever is in the open frame and open the next one, per aurora's contract:
//  - end_frame must NOT run if the matching begin_frame returned false
//  - a frame begun but not presentable must be DISCARDED, or the fifo grows unbounded
//  - aurora_update() is the event pump; without it the window/swapchain state never advances
//
// Getting this wrong made the per-frame staging buffer accumulate across frames instead of
// resetting: ~291 KB of stream per frame reached aurora's 48 MB limit after roughly 170 frames
// and aborted with "mapped ByteBuffer overflow".
void present_and_reopen(bool& frameActive) {
    ++g_present_count;   // presents, not game ticks: interpolation makes two per tick

    // SBR_PRESENT_TIMING=1: wall-clock gap between consecutive presents. A present COUNT of 60/s
    // says nothing about what reaches the display — if the two presents of a tick land back-to-back
    // and are then followed by a long gap, the eye sees 30fps however high the count reads. Only the
    // spacing distinguishes those, so measure it rather than infer it from the rate.
    if (std::getenv("SBR_PRESENT_TIMING")) {
        static int64_t prev = 0;
        static long n = 0;
        const int64_t now = now_ns();
        if (prev != 0) {
            const double ms = (double)(now - prev) / 1e6;
            static double acc_even = 0, acc_odd = 0; static long n_even = 0, n_odd = 0;
            if (n & 1) { acc_odd += ms; ++n_odd; } else { acc_even += ms; ++n_even; }
            if (++n % 120 == 0)
                lucent::info("ptime", "present gaps: alternating means {:.2f} ms / {:.2f} ms "
                                      "(even spacing = both ~16.7)",
                             n_even ? acc_even / n_even : 0.0, n_odd ? acc_odd / n_odd : 0.0);
        } else { ++n; }
        prev = now;
    }
    if (frameActive) {
        aurora_end_frame();
    } else {
        aurora_discard_frame();
    }

    // aurora_update returns the frame's event ARRAY; it is not a pop-one-at-a-time queue, so
    // calling it in a loop never terminates. The events MUST be inspected, not merely pumped:
    // AURORA_EXIT is how closing the window asks the program to stop, and discarding it left the
    // window uncloseable.
    const AuroraEvent* event = aurora_update();
    bool exit_requested = g_quit_requested != 0;
    while (event != nullptr && event->type != AURORA_NONE) {
        if (event->type == AURORA_EXIT) exit_requested = true;
        ++event;
    }
    if (exit_requested) {
        lucent::info("frame", "{} — shutting down",
                     g_quit_requested ? "signal received" : "window closed");
        aurora_shutdown();
        std::_Exit(0);
    }

    const bool wasActive = frameActive;
    frameActive = aurora_begin_frame();
    if (frameActive != wasActive) lucent::warn("frame", "aurora_begin_frame -> {}", frameActive);
}

void video_wait_for_retrace(CPUState& cpu) {
    report_app_state();
    report_mario_pos();
    // The probe's handlers run HERE, on the game thread at the frame boundary, which is the only
    // point guest memory is coherent. See probe_server.h.
    sb_probe_start();
    sb_probe_pump();

    // Audio: one service call per presented frame. Weak so the runtime links before the audio
    // subsystem exists — the seam is here so audio work never has to edit this file.
    sbr_audio_frame();
    sb_screen_effects_frame_end();   // roll the per-frame screen-effect set over

    // Native SDL3-GPU renderer (SBR_SDLGPU=1): draw the interpolated scene from the game's own
    // J3D geometry and its own projection. Still rendered to an OFFSCREEN target and read back —
    // aurora continues to drive the actual picture, so it stays a valid oracle while this is
    // scored against it.
    if (sbr_render_enabled() && sbr_render_init(640, 448)) {
        sbr_render_begin(0.10f, 0.40f, 0.80f, 1.0f);
        const float alpha = sbr_scene_render(sbr_scene_now(), sbr_scene_projection());
        sbr_render_end();

        static long n = 0;
        if (++n <= 4 || n % 120 == 0) {
            std::vector<uint8_t> px(640 * 448 * 4);
            long lit = 0;
            if (sbr_render_readback(px.data(), 640, 448))
                for (size_t i = 0; i < px.size(); i += 4)
                    if (px[i] != 26 || px[i + 1] != 102 || px[i + 2] != 204) ++lit;
            // Coverage is the honest bring-up signal: vertices submitted proves the frontend ran,
            // but only pixels differing from the clear prove the transform chain actually put
            // geometry on screen.
            float lo[3], hi[3], med = 0.0f;
            sbr_scene_translation_bounds(lo, hi, &med);
            lucent::info("nrender", "verts={} coverage={:.1f}% alpha={:.2f} drawables={} "
                                    "skinned-geom={} proj={} xyz=[{:.0f}..{:.0f} {:.0f}..{:.0f} "
                                    "{:.0f}..{:.0f}] medDist={:.0f}",
                         sbr_render_last_vertex_count(), 100.0 * (double)lit / (640.0 * 448.0),
                         alpha, sbr_scene_last_count(), sbr_scene_multislot_count(),
                         sbr_scene_has_projection() ? "yes" : "MISSING",
                         lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], med);
            sbr_scene_report_largest(5);
            if (const char* d = std::getenv("SBR_RENDER_DUMP")) sbr_render_dump(d);
        }
    }

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

    // The tick's scene is complete (the capture hooks ran during the game's draw). Rotate it so the
    // renderer has two snapshots to interpolate between, then open the next tick's recording.
    sbr_scene_end_tick();
    sbr_scene_begin_tick();

    gxfifo_build();

    // Rates, so "is it slow?" is measured rather than guessed. TICKS are game frames; PRESENTS
    // are what reaches the screen, and with 60fps interpolation there are two per tick — counting
    // only ticks made a working 60fps look like 30.
    {
        using clock = std::chrono::steady_clock;
        static auto t0 = clock::now();
        static long ticks = 0;
        static unsigned lastPresents = 0;
        if (++ticks % 30 == 0) {
            const auto now = clock::now();
            const double s = std::chrono::duration<double>(now - t0).count();
            lucent::debug("frame", "{:.1f} ticks/s, {:.1f} presents/s", 30.0 / s,
                          (double)(g_present_count - lastPresents) / s);
            t0 = now;
            lastPresents = g_present_count;
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

    gxfifo_send_last();
    present_and_reopen(s_frameActive);

    // PACING. Without this the recomp runs as fast as the host allows (measured ~157 fps against
    // the oracle's 30) — every animation, timer and physics step driven off the retrace count
    // runs at whatever speed the machine happens to manage, which is not the game.
    //
    // Pace to the number of retraces the GAME asked for this frame, taken from its own counter
    // (the same counter VIWaitForRetrace advances), not to a fixed 60Hz: a 30fps scene requests
    // two fields per frame and must be paced as two. Deadline-based rather than sleep-per-frame,
    // so a frame that overruns is absorbed by the next instead of compounding drift. This mirrors
    // sms-boot/runtime/frame_seam.cpp; SB_TURBO=1 disables it in both runtimes.
    {
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
        pace_fields(retraces);
    }
}

} // namespace

SB_OVERRIDE(0x802fc9a4u, video_wait_for_retrace, "JDrama::TVideo::waitForRetrace",
            "frame boundary: hand the collected GX stream to aurora and present")
