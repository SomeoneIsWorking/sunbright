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
#include "../frame_interp/effects.h"
#include "../runtime/native_render.h"
#include "../runtime/state_oracle.h"
#include "../runtime/scene.h"
#include "../frame_interp/stream_interp.h"
#include "../frame_interp/frame_interp.h"

// Declared rather than included: aurora's gfx headers are internal to the library, and the recomp
// links it statically so the symbol resolves directly. Same approach lerp60.cpp uses for aurora's
// tag counters.
namespace aurora::gfx {
void snap_next_interpolation();
namespace interp {
long tick_index();
}
} // namespace aurora::gfx
#include "../runtime/render_compare.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <intrinsics.h>
#include <lucent/log.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

extern "C" void func_802fc9a4(CPUState&);   // JDrama::TVideo::waitForRetrace
void sbr_mtx_report_index();
// Weak: resolves to a no-op until the audio subsystem provides it (see docs/audio/recomp_plan.md).
extern "C" __attribute__((weak)) void sbr_audio_frame();
extern "C" __attribute__((weak)) void sbr_audio_frame() {}

extern void gxfifo_build();
extern void gxfifo_send_last();
extern void gxfifo_send(const std::vector<u8>&);
extern const std::vector<u8>& gxfifo_last_frame();

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

// Frame-time split, reported under the `frame` channel. See the use site in video_wait_for_retrace.
int64_t g_seamEnterNs = 0;
int64_t g_lastPresentEndNs = 0;
double g_accGuestMs = 0, g_accOursMs = 0;
long g_frameSplitN = 0;

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

// How many NTSC fields the tick currently being presented is worth. Stashed before the present so
// the mid-tick pacing hook below can find it — aurora issues the second present from inside its own
// end_frame, where the retrace count is not in scope.
unsigned g_tickFields = 2;

// The guest retrace count stamped onto this tick's dumps, shared by the main present and the
// sub-frame present so both carry the same anchor.
u32 g_dumpGuestTick = 0;

// Deferred-present state for SBR_PRESENT_AFTER_COPY. The CPUState is needed by the sub-frame, and
// the seam has already returned by the time the copy is emitted, so it is carried across.
bool g_presentPending = false;
CPUState* g_pendingCpu = nullptr;
bool s_frameActive = true;   // main() opened the first frame

bool present_after_copy() {
    static const bool v = std::getenv("SBR_PRESENT_AFTER_COPY") != nullptr;
    return v;
}

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

} // namespace

// Called by aurora BETWEEN the two presents of an interpolated tick.
//
// WHY IT IS NEEDED. Sixty presents a second is not sixty frames a second to the eye. vsync is off,
// so a present reaches the screen when it is issued; with both presents issued back to back and the
// whole tick's sleep taken afterwards, the display gets two images a millisecond apart and then
// nothing for 33 ms. That is 30 fps with every frame sent twice — the count reads 60 and the motion
// does not. The interpolated image is a picture of the half-tick, so it has to be SHOWN at the
// half-tick.
//
// Deadline-derived, not a fixed sleep: g_nextDeadlineNs is the instant this tick was due to start,
// so the midpoint is that plus half the tick's own field count. A tick that has already overrun its
// midpoint does not sleep at all rather than pushing the tick further behind.
extern "C" void aurora_replay_midpoint() {
    // THE PRESENTATION FRAME. This function is called by aurora between the tick's two presents,
    // which makes it the only place in the frame loop that is genuinely mid-tick — so it is where
    // interpolation callbacks are dispatched (frame_interp.h). It runs before the sleep, so a
    // callback's work lands in the in-between image rather than after it has been shown.
    sb::frame_interp::present_interpolated_frame();
    if (turbo() || g_nextDeadlineNs == 0 || g_tickFields == 0) return;
    const int64_t midpoint = g_nextDeadlineNs + (int64_t)g_tickFields * kFieldNs / 2;
    const int64_t now = now_ns();
    if (now >= midpoint) return;
    const int64_t d = midpoint - now;
    {
        static double acc = 0;
        static long n = 0, skipped = 0;
        acc += (double)d / 1e6;
        if (++n % 120 == 0)
            lucent::debug("frame", "midpoint sleep avg {:.2f} ms over {} sleeps ({} ticks already "
                                   "past the midpoint), fields={}",
                          acc / (double)n, n, skipped, g_tickFields);
    }
    timespec ts{(time_t)(d / 1000000000LL), (long)(d % 1000000000LL)};
    nanosleep(&ts, nullptr);
}

extern "C" void sbr_interp60_restore();   // overrides/interp60_snapshot.cpp
extern "C" void sbr_interp60_subframe(CPUState& cpu, void (*present)(void));
extern "C" int  sbr_interp60_in_subframe();

namespace {

// Present whatever is in the open frame and open the next one, per aurora's contract:
//  - end_frame must NOT run if the matching begin_frame returned false
//  - a frame begun but not presentable must be DISCARDED, or the fifo grows unbounded
//  - aurora_update() is the event pump; without it the window/swapchain state never advances
//
// Getting this wrong made the per-frame staging buffer accumulate across frames instead of
// resetting: ~291 KB of stream per frame reached aurora's 48 MB limit after roughly 170 frames
// and aborted with "mapped ByteBuffer overflow".
void present_and_reopen(bool& frameActive) {
    ++g_present_count;   // PRESENTS, not game ticks — the two coincide today (one present per tick)

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

            // COMPARABLE WINDOW. The running means above are NOT an A/B instrument: they cover
            // every present since start, so a faster run reaches a different frame (and a
            // different part of the scene) by the time any given line prints. Measured A-vs-A
            // with an identical pad script, two runs of the same binary reported 25.4 ms and
            // 18.4 ms — a 38% spread from nothing but which frames each mean happened to span.
            // Any conclusion drawn by comparing two running means across runs is noise.
            //
            // This reports one mean over a FIXED present range, so two runs are compared at the
            // same N and the same point in the scene. Window ends are inclusive of neither edge
            // and both are overridable, because the right window depends on the scene.
            static const long lo = [] { const char* e = std::getenv("SBR_PTIME_LO"); return e ? std::atol(e) : 600; }();
            static const long hi = [] { const char* e = std::getenv("SBR_PTIME_HI"); return e ? std::atol(e) : 1200; }();
            static double wacc = 0; static long wn = 0; static bool wdone = false;
            if (!wdone && n >= lo && n < hi) { wacc += ms; ++wn; }
            if (!wdone && n >= hi) {
                wdone = true;
                if (wn == 0)
                    lucent::info("ptime", "COMPARABLE @ presents {}..{}: NO SAMPLES — the run never "
                                          "reached this window; this is not a fast frame time",
                                 lo, hi);
                else
                    lucent::info("ptime", "COMPARABLE @ presents {}..{} (N={}): {:.2f} ms/present "
                                          "— compare ONLY this line across runs",
                                 lo, hi, wn, wacc / (double)wn);
            }
        } else { ++n; }
        prev = now;
    }
    if (frameActive) {
        aurora_end_frame();
    } else {
        aurora_discard_frame();
    }

    // 60fps interpolation: put the guest transforms back AFTER the frame's GX stream has been
    // consumed. The restore used to sit at the next tick's first CUE_MOVE dispatch, which is too
    // early -- measured, not assumed: with the restore disabled entirely a substituted pose moved
    // 1,201,698 of 1,228,800 pixels, while with it at the movement dispatch it moved 0. The draw
    // for a tick evidently completes after that point, so the only unambiguous "after the render"
    // boundary is here.
    sbr_interp60_restore();

    // aurora_update returns the frame's event ARRAY; it is not a pop-one-at-a-time queue, so
    // calling it in a loop never terminates. The events MUST be inspected, not merely pumped:
    // AURORA_EXIT is how closing the window asks the program to stop, and discarding it left the
    // window uncloseable.
    // SBR_QUIT_AFTER=<presents>: stop once the run has produced what it was started for.
    //
    // A diagnostic run reaches its state of interest (SB_DUMP_FRAME_AFTER, a report cadence) and
    // then keeps running until whatever `timeout` the caller guessed. Every measurement run in the
    // 60fps arc paid that: a 300-present dump still burned 200 seconds of wall clock, because run
    // length was set by the timeout rather than by the thing being measured. The margin exists
    // because the dump's texture->buffer copy is mapped on the NEXT present, so quitting exactly at
    // the dump would truncate the file it was started to produce.
    {
        static const long quitAfter = [] {
            const char* e = std::getenv("SBR_QUIT_AFTER");
            return e ? std::atol(e) : 0L;
        }();
        if (quitAfter > 0 && (long)g_present_count >= quitAfter) {
            lucent::info("frame", "SBR_QUIT_AFTER={} reached at present {} — shutting down",
                         quitAfter, g_present_count);
            g_quit_requested = 1;
        }
    }

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

// WATCH the six known zero-decoding texture buffers, every present from boot. The question this
// answers: were they EVER non-zero? Aurora caches a texture at its FIRST draw and the port emits
// dataVersion 0 (never re-uploads), so if these buffers held content early and were zeroed later,
// aurora keeps rendering the real image from its cache while this port's honest re-decode sees
// zeros — which would explain "same binding, black here, bright there" with no state divergence.
static void texwatch_frame() {
    // Each buffer's REAL size, not a fixed 512-byte prefix. Sampling the first 512 bytes of an
    // 8192-byte I4 texture and concluding "zero" is the degenerate-sample trap: a texture whose
    // top rows are legitimately black would read as never-written for the whole run.
    static const u32 kWatch[] = {0x80a9bd20, 0x80abcc40, 0x80cf0ac0, 0x80cfafa0,
                                 0x80da3860, 0x80fea480};
    static const u32 kSize[]  = {1024,       1024,       8192,       2048,
                                 2048,       143360};
    static u8 was[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};   // 0xFF = not yet sampled
    for (int t = 0; t < 6; ++t) {
        if (!sb_ram_fast(kWatch[t])) continue;
        u32 sum = 0;
        for (u32 o = 0; o < kSize[t]; o += 4) sum |= sb_r32(kWatch[t] + o);
        const u8 now = sum != 0 ? 1 : 0;
        if (now != was[t]) {
            lucent::info("texwatch", "0x{:08x} {} at present {}", kWatch[t],
                         now ? "NON-ZERO" : "zero", g_present_count);
            was[t] = now;
        }
    }
}

void present_tail(CPUState& cpu);

void video_wait_for_retrace(CPUState& cpu) {
    texwatch_frame();
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

    // Frame sink consumers (A/B score, present smoothness). Armed HERE and not inside the native
    // renderer's block: smoothness measures AURORA's presented image and must work with the native
    // path off — it is the instrument for the 60fps interpolation work, which is an aurora change.
    sbr_compare_init();

    // Interpolation tag coverage, on a slow cadence. Inert unless SBR_LERP60 is set.
    if (sbr_lerp_enabled() && (g_present_count % 300) == 0) {
        sb::frame_interp::report();
        sbr_lerp_report_tag_coverage();
        sbr_camera_cut_report();
        sbr_afterimage_report();
    }

    // Native SDL3-GPU renderer (SBR_SDLGPU=1): draw the interpolated scene from the game's own
    // J3D geometry and its own projection. Still rendered to an OFFSCREEN target and read back —
    // aurora continues to drive the actual picture, so it stays a valid oracle while this is
    // scored against it.
    if (sbr_render_enabled() && sbr_render_init(640, 448)) {
        sbr_render_begin(0.10f, 0.40f, 0.80f, 1.0f);
        const float alpha = sbr_scene_render(sbr_scene_now(), sbr_scene_projection());
        sbr_render_end();

        sbr_compare_init();
        // The comparator wants a frame every SBR_AB_EVERY presents, which is a different (and
        // usually much denser) cadence than the human-readable report below — so it gets its own
        // readback rather than piggybacking on that one.
        if (sbr_compare_enabled()) {
            static std::vector<uint8_t> ab(640 * 448 * 4);
            if (sbr_render_readback(ab.data(), 640, 448))
                sbr_compare_submit_native(ab.data(), 640, 448, 26, 102, 204);
            // OPERATION ATTRIBUTION: re-render this same frame once per ablated operation and
            // submit each as a labelled variant. All of them are scored against the SAME aurora
            // frame as the baseline, so the ranked table is drift-free — unlike comparing the
            // means of two runs of different length, which is what previously misled this arc.
            if (sbr_compare_ablate_enabled()) {
                // Checksum every variant INCLUDING the control, against the baseline's. The
                // control renders the real pipeline, so its checksum MUST equal the baseline's.
                // If it does not, re-rendering the same frame is not reproducible and the whole
                // attribution table is void — this is the check that says so out loud rather than
                // letting a broken sweep read as a finding.
                auto sum = [](const std::vector<uint8_t>& p) {
                    unsigned long long h = 1469598103934665603ULL;
                    for (size_t i = 0; i < p.size(); i += 4)
                        h = (h ^ (p[i] + 3u * p[i + 1] + 7u * p[i + 2])) * 1099511628211ULL;
                    return h;
                };
                const unsigned long long base = sum(ab);
                auto dump = [](const char* path, const std::vector<uint8_t>& p) {
                    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(p.data(), 1, p.size(), f);
                                                            std::fclose(f); }
                };
                // Only on a frame that HAS geometry: the first presents are the loading screen,
                // where every ablation of an empty frame is trivially identical — a degenerate
                // input that would read as "the sweep works" when it proves nothing.
                static long once = 0;
                const bool tell = sbr_render_last_vertex_count() > 1000 && once++ < 2;

                if (tell) dump("scratch/bin/sweep_baseline.rgba", ab);
                for (int a = 1; a < sbr_render_ablation_count(); ++a)
                    if (sbr_render_ablation_render(a) && sbr_render_readback(ab.data(), 640, 448)) {
                        if (tell && a == 9) dump("scratch/bin/sweep_pinunit1.rgba", ab);
                        if (tell)
                            lucent::info("ab", "   sweep checksum: baseline {:016x}  {} {:016x}{}",
                                         base, sbr_render_ablation_name(a), sum(ab),
                                         (sum(ab) == base) ? "  (identical)" : "");
                        sbr_compare_submit_variant(a, sbr_render_ablation_name(a), ab.data(),
                                                   640, 448);
                    }
            }
        }

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
                                    "skinned-geom={} batches={} proj={} xyz=[{:.0f}..{:.0f} {:.0f}..{:.0f} "
                                    "{:.0f}..{:.0f}] medDist={:.0f}",
                         sbr_render_last_vertex_count(), 100.0 * (double)lit / (640.0 * 448.0),
                         alpha, sbr_scene_last_count(), sbr_scene_multislot_count(),
                         sbr_render_last_batch_count(),
                         sbr_scene_has_projection() ? "yes" : "MISSING",
                         lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], med);
            sbr_scene_report_largest(5);
            sbr_scene_report_zmodes();
            sbr_scene_report_2d();
            sbr_scene_report_alpha();
            sbr_render_report_formats();
            sbr_render_recheck_black();
            sbr_state_oracle_report();
            sbr_gxfifo_report_bp_writes();
            sbr_compare_report_attribution();
            // Attribute the black background to an actual batch, at a pixel well inside it.
            if (std::getenv("SBR_BLACK_OWNER") != nullptr) sbr_render_report_black_owner(320, 60);
            if (const char* d = std::getenv("SBR_DUMP_COPY"))
                sbr_render_dump_copy(0x80fea480u, d);
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
    // WHERE THE FRAME TIME GOES. "It runs at N fps" does not say whether the cost is the game's own
    // recompiled code or our rendering, and those have nothing to do with each other: no amount of
    // render optimisation helps if the tick is spent in guest logic, and interpolated 60fps is only
    // worth having BECAUSE it does not re-run that logic. Split at the seam — everything since the
    // last present is the guest's tick, everything from here to the next present is ours.
    if (g_lastPresentEndNs != 0) {
        g_accGuestMs += (double)(now_ns() - g_lastPresentEndNs) / 1e6;
    }
    g_seamEnterNs = now_ns();

    // RE-ENTRY FROM A SUB-FRAME. The sub-frame calls the game's endRendering purely to emit the
    // EFB->XFB copy, and endRendering's first act is to wait for a retrace — which lands here. Doing
    // the seam's own work now would end the frame the sub-frame is still assembling and present it
    // early. Return immediately and let the copy be the only thing that happens.
    if (sbr_interp60_in_subframe()) return;

    // Let the game do its own frame bookkeeping first.
    func_802fc9a4(cpu);

    // The tick's scene is complete (the capture hooks ran during the game's draw). Rotate it so the
    // renderer has two snapshots to interpolate between, then open the next tick's recording.
    sbr_mtx_report_index();
    sbr_scene_end_tick();
    sbr_scene_begin_tick();

    // NOTE: the stream is built and sent in present_tail(), not here. Under
    // SBR_PRESENT_AFTER_COPY the seam returns before the game issues its GXCopyDisp, so building
    // here would close the stream BEFORE the copy command exists — the copy would then land in the
    // NEXT tick's stream and every present would render a frame whose copy had not been emitted.

    // Rates, so "is it slow?" is measured rather than guessed. TICKS are game frames; PRESENTS
    // are what reaches the screen. Today that is one per tick; counting presents rather than ticks
    // keeps the number meaningful if that ever stops being true.
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
    // (s_frameActive is at namespace scope: the seam is split across two functions when
    //  SBR_PRESENT_AFTER_COPY defers the present past the game's copy.)

    // The camera this tick's draws were built with, for interpolation. Last thing in the stream, so
    // it is the settled value rather than the previous tick's.

    // A tick in which the game WARPED the camera has no in-between to show. Tell aurora to present
    // this tick exactly rather than a halfway viewpoint the game never simulated. Read here, before
    // the present, so the flag covers exactly the tick whose draws are about to be emitted.

    // The mid-tick pacing hook needs this tick's field count, and aurora issues the second present
    // from inside its own end_frame where that number is out of scope. The count is only known
    // AFTER the present (it is a delta on the game's retrace counter), so use the PREVIOUS tick's —
    // it is the same number on every tick of a steady scene, and being one tick stale costs at most
    // a slightly mistimed midpoint on the frame where the rate changes. Seeded to 2 because a
    // 30fps scene requests two fields per tick, so the very first tick paces correctly rather than
    // at double rate.
    // SBR_PRESENT_AFTER_COPY=1: defer the present until after the game's EFB->XFB copy.
    //
    // TDisplay::endRendering calls waitForRetrace FIRST and IssueGXCopyDisp SECOND, and this
    // function IS waitForRetrace — so presenting here presents a frame whose copy has not been
    // issued yet. That has been the behaviour all along and it looked correct only because the
    // display kept showing the previously copied XFB, one frame stale. It became visible the moment
    // a sub-frame issued a copy of its own: the main presents came back black.
    //
    // waitForRetrace has exactly ONE call site (JDRDisplay.cpp:38), so deferring to the end of
    // endRendering is a single-path change rather than a guess about who else might call it.
    if (present_after_copy()) {
        g_presentPending = true;
        g_pendingCpu = &cpu;
        return;                 // the rest of the seam runs from sbr_frame_present_now()
    }

    present_tail(cpu);
}

// The part of the seam that must happen AFTER the game's EFB->XFB copy when SBR_PRESENT_AFTER_COPY
// is set: the present itself, the interpolated sub-frame, and the frame-time bookkeeping that
// brackets them. Factored out rather than duplicated so the two placements cannot drift apart.
void present_tail(CPUState& cpu) {
    // Close and send THIS tick's stream. Deliberately here and not in the seam: when the present is
    // deferred past the game's EFB->XFB copy, the copy command is emitted after the seam returns, so
    // a stream closed in the seam would not contain it.
    gxfifo_build();
    // ONE SIMULATION TICK ENDS HERE. begin_sim_tick() clears the interpolation-callback registry,
    // so it must run once per tick and before anything registers for the NEXT in-between frame.
    sb::frame_interp::begin_sim_tick();
    if (sbr_lerp_enabled()) sbr_afterimage_tick();
    if (sbr_lerp_enabled()) sbr_gxfifo_view_matrix();
    // The camera cut goes through the unified API rather than straight to aurora's snap: a cut is
    // "present this tick exactly", which is a statement about the whole frame and not only about
    // the renderer's matrix rewrite. Routing it here is what lets anything else that must be exact
    // on a cut — an effect, a UI element — see the same signal instead of re-deriving it.
    if (sbr_lerp_enabled() && sbr_camera_cut_take()) sb::frame_interp::request_presentation_sync();
    gxfifo_send_last();

    // Label this present for the dump series. The sub-frame below issues a SECOND present per
    // tick, and a dump series with no record of which file is which has to be identified by
    // inference — which has already produced two wrong readings in this arc. The runtime knows the
    // answer, so it says it.
    //
    // AND THE GUEST TICK, because the present INDEX is not a moment. Configurations of the
    // interpolation reach different game states by the same present number — measured, three
    // configurations dumped at present 60 sat at tick motions of 10.6, 76.5 and 27.7 — so any
    // cross-configuration comparison keyed on the index is comparing two different moments and
    // reporting the difference as a finding. The game's own retrace counter is the anchor the two
    // runs genuinely share, and it costs one word in a filename to carry it.
    const u32 gtickAddr = (u32)cpu.gpr[13] - 22768;
    const u32 gtick = sb_ram_fast(gtickAddr) ? sb_r32(gtickAddr) : 0u;
    g_dumpGuestTick = gtick;
    char tag[32];
    std::snprintf(tag, sizeof tag, "main-t%u", gtick);
    aurora_set_dump_tag(tag);
    present_and_reopen(s_frameActive);

    // GAME-NATIVE 60fps: the interpolated sub-frame.
    //
    // Placed AFTER the tick's own present, which is where it belongs temporally rather than where
    // it is merely convenient. The draw lists run at the START of a direct() call and PreEntry runs
    // at the END (measured, SBR_INTERP60_LISTS), so the frame just presented was drawn from the
    // pose entered at tick N-1. The sub-frame built here is lerp(N-1, N, alpha), so what reaches
    // the display is N-1, mid, N, mid, N+1 — in order.
    //
    // The callback closes the sub-frame's GX stream exactly the way the tick's own is closed. It
    // must not be a partial imitation: a sub-frame assembled by a different path would diverge from
    // the real frame for reasons that have nothing to do with interpolation.
    // SBR_INTERP60_CENSUS also reaches here: the motion census lives at the top of that function and
    // must be obtainable from a run that interpolates nothing, because that run is the baseline.
    if (std::getenv("SBR_INTERP60") || std::getenv("SBR_INTERP60_CENSUS")) {
        static bool* s_active = &s_frameActive;
        s_active = &s_frameActive;
        sbr_interp60_subframe(cpu, [] {
            // Does the sub-frame's own stream actually reach the present?
            //
            // The runtime-labelled dump series says the presented sub-frame is pixel-identical to
            // the main frame before it, which is what "the main frame was presented twice" looks
            // like. gxfifo_build() RETURNS EARLY when g_out is empty, leaving g_last holding the
            // previous (main) frame — and gxfifo_send_last() would then re-send that. So the sizes
            // either side of the build are the discriminator, and they are cheap to print.
            const uint32_t before = sbr_gxfifo_stream_pos();
            gxfifo_build();
            const uint32_t after = sbr_gxfifo_stream_pos();
            static long n = 0;
            if (++n <= 6 || (n % 600) == 0)
                lucent::info("i60sub", "sub-frame present #{}: g_out {} KB before build, {} KB "
                                       "after; g_last now {} KB {}",
                             n, before >> 10, after >> 10, gxfifo_last_frame().size() >> 10,
                             before == 0 ? "<-- EMPTY: the re-issue emitted NOTHING, so the main "
                                           "frame's stream is what gets re-sent" : "");
            gxfifo_send_last();
            // Same guest tick as the main present above: the sub-frame is an EXTRA present inside
            // one tick, not a tick of its own, and stamping it with a tick of its own would make
            // the anchor lie in exactly the way the present index already does.
            char stag[32];
            std::snprintf(stag, sizeof stag, "sub-t%u", g_dumpGuestTick);
            aurora_set_dump_tag(stag);
            // The half-tick image has to be SHOWN at the half tick; both presents issued back to
            // back are 30fps with every frame sent twice, however high the present count reads.
            aurora_replay_midpoint();
            present_and_reopen(*s_active);
        });
    }

    // Sampled AFTER the present, and stamped with aurora's OWN tick counter rather than one derived
    // from the present count. The two instruments must be joined on a number they genuinely share:
    // presents run at two per tick under replay, so a derived index would drift silently and any
    // correlation drawn from it would be worthless — the failure this project has hit repeatedly.
    if (sbr_lerp_enabled()) sbr_camera_mode_tick(aurora::gfx::interp::tick_index());

    // Close the frame-time split opened at the top of this function.
    g_lastPresentEndNs = now_ns();
    if (g_seamEnterNs != 0) {
        g_accOursMs += (double)(g_lastPresentEndNs - g_seamEnterNs) / 1e6;
        if (++g_frameSplitN % 60 == 0)
            lucent::debug("frame", "per tick: guest logic {:.1f} ms, present+render {:.1f} ms "
                                   "(pacing sleep excluded from both)",
                          g_accGuestMs / (double)g_frameSplitN, g_accOursMs / (double)g_frameSplitN);
    }

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
        // Remembered for the NEXT tick's midpoint pacing (see aurora_replay_midpoint).
        g_tickFields = retraces;
        pace_fields(retraces);
    }
}

} // namespace

// Called from the TDisplay::endRendering override once the game's own EFB->XFB copy has been
// issued. Only does anything when SBR_PRESENT_AFTER_COPY deferred the present.
extern "C" void sbr_frame_present_now() {
    if (!g_presentPending || g_pendingCpu == nullptr) return;
    g_presentPending = false;
    present_tail(*g_pendingCpu);
}

SB_OVERRIDE(0x802fc9a4u, video_wait_for_retrace, "JDrama::TVideo::waitForRetrace",
            "frame boundary: hand the collected GX stream to aurora and present")
