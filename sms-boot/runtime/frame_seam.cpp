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
void aurora_host_alloc_push(void) { sb_host_alloc_push(); }
void aurora_host_alloc_pop(void) { sb_host_alloc_pop(); }
}

namespace {

// NTSC field period: 60000/1001 fields per second.
constexpr int64_t kFieldNs = 1000000000LL * 1001 / 60000;

bool s_frameOpen = false;
int64_t s_nextDeadlineNs = 0;

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

// SB_PROFILE=N: every N presents, print a rolling mean of each frame phase's
// wall-clock cost (μs). Phases: game = time the GAME spent between presents
// (its own logic + GX emission), endframe = aurora_end_frame (GX fifo drain +
// wgpu render + present), events = aurora_update, begin = aurora_begin_frame.
// The "game" slice is measured present-exit(N-1) -> present-enter(N). Turbo
// removes pacing so these are pure work costs. Default off.
struct Profiler {
    int period = 0;         // 0 = disabled
    long n = 0;
    int64_t lastExit = 0;   // present-exit timestamp of previous frame
    double sGame = 0, sEnd = 0, sEvt = 0, sBeg = 0;
    static Profiler& get() {
        static Profiler p = [] {
            Profiler q;
            const char* e = std::getenv("SB_PROFILE");
            q.period = (e && e[0]) ? std::atoi(e) : 0;
            return q;
        }();
        return p;
    }
};

} // namespace

extern "C" {

// Open the first Aurora frame. Called once from main() after aurora_initialize,
// before any game code runs.
void sb_frame_seam_start(void) {
    s_frameOpen = aurora_begin_frame();
    s_nextDeadlineNs = now_ns() + kFieldNs;
}

void sb_frame_present(unsigned retraces) {
    if (trace_seq_on()) {
        std::fprintf(stderr, "[trace] seq=%lu present-enter retrace=%u retraces_arg=%u\n",
                     (unsigned long)sb_trace_seq(), VIGetRetraceCount(), retraces);
    }
    Profiler& prof = Profiler::get();
    int64_t tEnter = prof.period ? now_ns() : 0;

    sb_host_alloc_push();

    if (s_frameOpen) {
        aurora_end_frame();
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

    int64_t tEndDone = prof.period ? now_ns() : 0;

    const AuroraEvent* event = aurora_update();
    bool exit_requested = false;
    while (event != nullptr && event->type != AURORA_NONE) {
        if (event->type == AURORA_EXIT) {
            exit_requested = true;
        }
        ++event;
    }
    if (exit_requested) {
        std::fprintf(stderr, "[sms-boot] window closed, exiting\n");
        aurora_shutdown();
        _exit(0); // no static-dtor teardown: game/Dawn statics are not unwind-safe
    }

    int64_t tEvtDone = prof.period ? now_ns() : 0;

    s_frameOpen = aurora_begin_frame();
    if (trace_seq_on()) {
        std::fprintf(stderr, "[trace] seq=%lu aurora-begin-frame retrace=%u\n",
                     (unsigned long)sb_trace_seq(), VIGetRetraceCount());
    }
    sb_host_alloc_pop();

    if (prof.period) {
        int64_t tBegDone = now_ns();
        if (prof.lastExit != 0) prof.sGame += (tEnter - prof.lastExit) / 1000.0;
        prof.sEnd += (tEndDone - tEnter) / 1000.0;
        prof.sEvt += (tEvtDone - tEndDone) / 1000.0;
        prof.sBeg += (tBegDone - tEvtDone) / 1000.0;
        if (++prof.n >= prof.period) {
            double d = prof.n;
            std::fprintf(stderr,
                "[profile] frames=%ld avg μs: game=%.0f endframe=%.0f events=%.0f begin=%.0f  total=%.0f (%.1f fps-equiv)\n",
                prof.n, prof.sGame / d, prof.sEnd / d, prof.sEvt / d, prof.sBeg / d,
                (prof.sGame + prof.sEnd + prof.sEvt + prof.sBeg) / d,
                1e6 / ((prof.sGame + prof.sEnd + prof.sEvt + prof.sBeg) / d));
            prof.n = 0; prof.sGame = prof.sEnd = prof.sEvt = prof.sBeg = 0;
        }
    }

    // Advance the SDK retrace counter by the fields this frame covers so the
    // game's own pacing math (TVideo::mNextRetraceIndex) stays consistent.
    if (retraces == 0) retraces = 1;
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

    if (!turbo()) {
        s_nextDeadlineNs += (int64_t)retraces * kFieldNs;
        int64_t now = now_ns();
        if (now < s_nextDeadlineNs) {
            timespec ts{ (time_t)((s_nextDeadlineNs - now) / 1000000000LL),
                         (long)((s_nextDeadlineNs - now) % 1000000000LL) };
            nanosleep(&ts, nullptr);
        } else if (now - s_nextDeadlineNs > 4 * kFieldNs) {
            // Fell far behind (load hitch): resynchronize instead of sprinting.
            s_nextDeadlineNs = now;
        }
    }

    // Mark the present-exit instant for the next frame's "game" slice (game
    // logic + GX emission between two presents). After pacing so a paced run
    // still attributes only real work to "game", not the sleep.
    if (prof.period) prof.lastExit = now_ns();
}

} // extern "C"
