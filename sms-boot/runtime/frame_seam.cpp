// frame_seam.cpp — the single-threaded frame boundary: sb_frame_present().
//
// The game owns the (only) thread; Aurora is IO called from inside it. The
// seam sits in JDrama::TVideo::waitForRetrace (the game's own once-per-frame
// scan-out point, reached from TDisplay::endRendering), NOT in the
// VIWaitForRetrace SDK call — the game also spins on VIWaitForRetrace from
// load-polling and TV-mode settle loops, where presenting a half-built GX
// fifo would render garbage. One sb_frame_present(retraces) =
//   end the open Aurora frame (drain GX fifo, render, present)
//   -> pump window/input events
//   -> begin the next frame
//   -> pace to retraces * one NTSC field (16.683 ms) of wall clock.
//
// Pacing is wall-clock, not host-vsync: the game asks for N retraces per
// frame (SMS gameplay runs at 30 fps = 2 fields) and the host refresh rate
// is arbitrary (60/144/240 Hz), so present-blocking cannot provide GC frame
// timing. SB_TURBO=1 disables pacing (run as fast as the host can).
//
// Everything Aurora does inside this seam (shader compilation, Dawn command
// submission, SDL event handling) allocates host C++ memory, and this thread
// is marked as the game thread (plain `new` routes to the JKR heap), so the
// whole seam runs under the sb_host_alloc gate.

#include <aurora/aurora.h>
#include <aurora/event.h>

#include <sunbright/native_render/semantic_frame_bridge.h>

#include "semantic_render.h"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>

extern "C" {
void sb_host_alloc_push(void);
void sb_host_alloc_pop(void);
void sb_watchdog_kick(void);
void sb_audio_frame(void);
uint32_t VIGetRetraceCount(void);
void VIWaitForRetrace(void);

// Shared cross-instrument sequence counter (runtime/trace_seq.cpp).
// SB_TRACE_SEQ=1: stamp present-boundary entry/exit so they interleave with
// the plist-order/proj/drawbuf-flush logs on one global order, not just a
// retrace stamp (see trace_seq.cpp for why retrace alone doesn't suffice).
uint64_t sb_trace_seq(void);

// Headless scripted controller input (runtime/pad_script.cpp), driven by
// SB_PAD_SCRIPT. No-op when unset. Feeds Aurora's virtual-pad seam
// (PADSetVirtualStatus), which composes with keyboard/gamepad input.
void sb_pad_script_tick(uint32_t retrace_count);

// Aurora's weak host-alloc hooks (lib/dolphin/dvd) resolve to the JKR gate:
// DVD entry points allocate C++ objects (file handles, FST path strings)
// whose lifetime must not be tied to whatever JKR heap is current.
void aurora_host_alloc_push(void) {
    sb_host_alloc_push();
}
void aurora_host_alloc_pop(void) {
    sb_host_alloc_pop();
}
}

namespace {

// NTSC field period: 60000/1001 fields per second.
constexpr int64_t kFieldNs = 1000000000LL * 1001 / 60000;
constexpr std::uint64_t kMaxQuitAfter = 10'000;

bool s_frameOpen = false;
int64_t s_nextDeadlineNs = 0;
std::uint64_t s_presentCount = 0;

std::uint64_t quit_after() {
    static const std::uint64_t value = [] {
        const char* text = std::getenv("SB_QUIT_AFTER");
        if (text == nullptr || text[0] == '\0')
            return std::uint64_t{0};
        const char* end = text;
        while (*end != '\0')
            ++end;
        std::uint64_t parsed = 0;
        const auto result = std::from_chars(text, end, parsed, 10);
        if (result.ec != std::errc{} || result.ptr != end || parsed == 0 ||
            parsed > kMaxQuitAfter) {
            std::fprintf(stderr,
                         "[sms-boot] SB_QUIT_AFTER requires an integer from 1 through 10000\n");
            std::abort();
        }
        return parsed;
    }();
    return value;
}

int64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

bool turbo() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SB_TURBO");
        v = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return v == 1;
}

bool trace_seq_on() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SB_TRACE_SEQ");
        v = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return v == 1;
}

} // namespace

extern "C" {

void sb_frame_seam_configure(void) {
    // Force strict run-limit parsing before Aurora or either GPU device is initialized.
    (void)quit_after();
}

// Open the first Aurora frame. Called once from main() after aurora_initialize,
// before any game code runs.
void sb_frame_seam_start(void) {
    s_frameOpen = aurora_begin_frame();
    auto& semanticFrame = sb::native_render::semantic_frame_bridge();
    if (!semanticFrame.begin()) {
        std::fprintf(stderr, "[sms-boot] semantic frame begin failed: %s\n",
                     semanticFrame.last_error());
        std::abort();
    }
    s_nextDeadlineNs = now_ns() + kFieldNs;
}

void sb_frame_present(unsigned retraces) {
    if (trace_seq_on()) {
        std::fprintf(stderr, "[trace] seq=%lu present-enter retrace=%u retraces_arg=%u\n",
                     (unsigned long)sb_trace_seq(), VIGetRetraceCount(), retraces);
    }
    sb_host_alloc_push();

    auto& semanticFrame = sb::native_render::semantic_frame_bridge();
    if (!semanticFrame.seal()) {
        std::fprintf(stderr, "[sms-boot] semantic frame seal failed: %s\n",
                     semanticFrame.last_error());
        std::abort();
    }
    // The semantic target is offscreen and independent of Aurora surface availability. Every
    // sealed simulation frame must be consumed exactly once even while the visible window is
    // unavailable; otherwise a minimized audit could report success after encoding nothing.
    if (!sb_semantic_render_consume()) {
        std::fprintf(stderr, "[sms-boot] offscreen semantic frame failed: %s\n",
                     sb_semantic_render_last_error());
        std::abort();
    }

    if (s_frameOpen) {
        aurora_end_frame();
        ++s_presentCount;
        if (trace_seq_on()) {
            std::fprintf(stderr, "[trace] seq=%lu aurora-end-frame retrace=%u\n",
                         (unsigned long)sb_trace_seq(), VIGetRetraceCount());
        }
    } else {
        // Surface unpresentable (minimized): the frame's GX commands were
        // queued but never begun; drop them so the fifo doesn't grow.
        aurora_discard_frame();
        if (trace_seq_on()) {
            std::fprintf(stderr, "[trace] seq=%lu aurora-discard-frame retrace=%u\n",
                         (unsigned long)sb_trace_seq(), VIGetRetraceCount());
        }
    }

    const AuroraEvent* event = aurora_update();
    bool exit_requested = false;
    while (event != nullptr && event->type != AURORA_NONE) {
        if (event->type == AURORA_EXIT) {
            exit_requested = true;
        }
        ++event;
    }
    const bool quitAfterReached = quit_after() != 0 && s_presentCount >= quit_after();
    exit_requested = exit_requested || quitAfterReached;
    if (exit_requested) {
        std::fprintf(stderr, "[sms-boot] %s, exiting\n",
                     quitAfterReached ? "SB_QUIT_AFTER reached" : "window closed");
        if (quitAfterReached && !sb_semantic_render_validate()) {
            std::fprintf(stderr, "[sms-boot] bounded semantic audit failed: %s\n",
                         sb_semantic_render_last_error());
            std::abort();
        }
        if (!sb_semantic_render_shutdown()) {
            std::fprintf(stderr, "[sms-boot] semantic renderer shutdown failed: %s\n",
                         sb_semantic_render_last_error());
            std::abort();
        }
        aurora_shutdown();
        _exit(0); // no static-dtor teardown: game/Dawn statics are not unwind-safe
    }

    s_frameOpen = aurora_begin_frame();
    if (trace_seq_on()) {
        std::fprintf(stderr, "[trace] seq=%lu aurora-begin-frame retrace=%u\n",
                     (unsigned long)sb_trace_seq(), VIGetRetraceCount());
    }
    if (!semanticFrame.begin()) {
        std::fprintf(stderr, "[sms-boot] semantic frame begin failed: %s\n",
                     semanticFrame.last_error());
        std::abort();
    }
    sb_host_alloc_pop();

    // Advance the SDK retrace counter by the fields this frame covers so the
    // game's own pacing math (TVideo::mNextRetraceIndex) stays consistent.
    if (retraces == 0)
        retraces = 1;
    for (unsigned i = 0; i < retraces; ++i) {
        VIWaitForRetrace();
        sb_pad_script_tick(VIGetRetraceCount());
    }

    sb_watchdog_kick();
    sb_audio_frame();

    if (trace_seq_on()) {
        std::fprintf(stderr, "[trace] seq=%lu present-exit retrace=%u\n",
                     (unsigned long)sb_trace_seq(), VIGetRetraceCount());
    }

    // A CEILING ON GPU SUBMISSION THAT APPLIES EVEN IN TURBO. SB_TURBO exists to stop pacing the
    // GAME to the wall clock; what it also did was remove the only limit on how fast this process
    // hands work to the GPU. Aurora replays the whole GX stream and presents once per call here, so
    // an unpaced run submitted thousands of frames a second back to back and left the graphics ring
    // no gap for the compositor. On 2026-08-12 that helped make this machine unusable — see
    // debug_journal/2026-08-12_gpu_hang_guards.md and the matching ceiling in the recomp's
    // native_frame.cpp. Fast-forwarding does not need a frame per CPU-microsecond; the guest still
    // runs unpaced between presents, only the submission rate is bounded. SB_MAX_PRESENT_HZ=0
    // disables it, and has to be typed to do so.
    {
        static const int64_t s_minGapNs = [] {
            const char* e = std::getenv("SB_MAX_PRESENT_HZ");
            const double hz = e != nullptr ? std::strtod(e, nullptr) : 120.0;
            return hz > 0.0 ? (int64_t)(1e9 / hz) : (int64_t)0;
        }();
        static int64_t s_nextSubmitNs = 0;
        if (s_minGapNs != 0) {
            const int64_t now = now_ns();
            if (s_nextSubmitNs != 0 && now < s_nextSubmitNs) {
                const int64_t d = s_nextSubmitNs - now;
                timespec ts{(time_t)(d / 1000000000LL), (long)(d % 1000000000LL)};
                nanosleep(&ts, nullptr);
            }
            s_nextSubmitNs = (s_nextSubmitNs == 0 || now > s_nextSubmitNs + 4 * s_minGapNs)
                                 ? now + s_minGapNs
                                 : s_nextSubmitNs + s_minGapNs;
        }
    }

    if (!turbo()) {
        s_nextDeadlineNs += (int64_t)retraces * kFieldNs;
        int64_t now = now_ns();
        if (now < s_nextDeadlineNs) {
            timespec ts{(time_t)((s_nextDeadlineNs - now) / 1000000000LL),
                        (long)((s_nextDeadlineNs - now) % 1000000000LL)};
            nanosleep(&ts, nullptr);
        } else if (now - s_nextDeadlineNs > 4 * kFieldNs) {
            // Fell far behind (load hitch): resynchronize instead of sprinting.
            s_nextDeadlineNs = now;
        }
    }
}

} // extern "C"
