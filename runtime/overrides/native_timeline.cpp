// native_timeline.cpp — SbTimeline: a host-clock-paced event timeline that OWNS the
// AudioDMACallback cadence (opt-in, SUNBRIGHT_OWN_TIME=1). Default OFF = unchanged
// (Dolphin's CoreTiming owns everything).
//
// ─────────────────────────────────────────────────────────────────────────────
// WHY (design recap — full RE in docs/re_notes/coretiming_governor.md)
// ─────────────────────────────────────────────────────────────────────────────
// Today emulated time is a *negotiation*: our host-clock governor (dolphin_hook.cpp
// charge_guest_time / sb_time_ahead) drives the PPC downcount and calls
// CoreTiming::Advance, but Dolphin still owns the event heap, the per-event
// `period - cycles_late` reschedule arithmetic, and the firing. Every CoreTiming-vs-
// governor conflict bug we hit (dead-jingle/AID starve, time-parked 0-cycle device
// events, the Throttle 6-12fps crawl, frame jitter) lives at exactly that split seam.
//
// The design doc's recommended FIRST step (lowest risk / highest payoff): own ONLY the
// AudioDMACallback firing cadence on the audio servo. We already own the AID *raise*
// (overrides/aid_native.cpp) and the host audio sink is already our master clock; this
// removes the LAST audio-timing dependence on CoreTiming (the §2.4 starve class).
// Dolphin's AudioDMACallback BODY is left UNCHANGED — UpdateAudioDMA still pushes audio
// and the event still self-reschedules with `period - cycles_late`; we only change WHEN
// it fires (from OUR host-clock heap instead of Dolphin's CoreTiming heap).
//
// VI / DSP / DI / decrementer stay on Dolphin's heap — CoreTiming is load-bearing for
// boot (arming order, TB coherence). This is deliberately conservative: own one event.
//
// ─────────────────────────────────────────────────────────────────────────────
// REQUIRED SEAMS — fork edits (this IS our fork, but they are documented, NOT applied
// here; see the `// ==== REQUIRED SEAM ====` block at the bottom of this file). The
// timeline talks to Dolphin only through the function pointers below, so this file
// compiles/links standalone with the seams unapplied (flag-off path is then inert).
// ─────────────────────────────────────────────────────────────────────────────
//   1. CoreTiming.h `struct EventType`: add `bool owned = false;`
//   2. SystemTimers::Init: after registering m_event_type_audio_dma, if
//      sb_own_time_enabled() set `m_event_type_audio_dma->owned = true;`
//   3. CoreTimingManager::ScheduleEvent: for an owned event_type on the CPU thread,
//      forward to sb_timeline_schedule(cycles_into_future, event_type, userdata, GetTicks())
//      and RETURN (do not push onto the heap).
//   4. CoreTimingManager::Init (or first Advance): call sb_timeline_set_fire_cb(<thunk>)
//      once, where the thunk does `static_cast<EventType*>(et)->callback(system, ud, late)`.
//   5. dolphin_hook.cpp governor catch-up site (charge_guest_time, after the batched
//      Advance computes the host-clock target): call sb_timeline_advance_to(target).
//
// ─────────────────────────────────────────────────────────────────────────────
// VERIFY RECIPE (headless, A/B vs OFF)
// ─────────────────────────────────────────────────────────────────────────────
//   ON : SUNBRIGHT_OWN_TIME=1 SUNBRIGHT_DUMP_AUDIO=1 SUNBRIGHT_DBG_NAUDIO=1 \
//        SUNBRIGHT_HEADLESS=1 ./build/sunbright   (then kill after ~60s)
//   OFF: same WITHOUT SUNBRIGHT_OWN_TIME  (baseline — current working path)
//   Compare:
//     - audio plays: scratch/wav/native_dsp.wav RMS per second (tools/audio/wav_rms.py)
//       — must match OFF (sustained, no new underruns in DBG_NAUDIO).
//     - push rate ≈ 32028 Hz steady (DBG_NAUDIO per-second), same as OFF.
//     - frame pacing unchanged (watchdog VI-field cadence; no boot regression).
//     - SbTimeline DBG (SUNBRIGHT_DBG_TIMELINE=1): per-second fired-count ≈ the
//       AudioDMA period rate, advance_to never stalls.
//   PASS = ON behaves like OFF for audio+pacing, no boot regression over a full
//   boot→gameplay run. Then (and only then) consider widening to VI/DSP (step 2 of
//   the migration in the design doc).
//
// Syntax-checked standalone: g++ -std=c++20 -fsyntax-only runtime/overrides/native_timeline.cpp
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <algorithm>
#include <ctime>

namespace {

// ── Opt-in flag ──────────────────────────────────────────────────────────────
// Cached once. Default OFF: the fork seams gate on this too, so with the flag off the
// AudioDMA event is never marked owned, ScheduleEvent never forwards here, and
// advance_to has an empty queue — the path is fully inert.
bool own_time_enabled_impl() {
    static const bool on = [] {
        const char* e = getenv("SUNBRIGHT_OWN_TIME");
        return e && *e && *e != '0';
    }();
    return on;
}
bool timeline_dbg() {
    static const bool on = getenv("SUNBRIGHT_DBG_TIMELINE") != nullptr;
    return on;
}

// ── The owned event ──────────────────────────────────────────────────────────
// due_ticks is on the SAME clock as CoreTiming's global_timer (PPC core cycles), so the
// governor's host-clock `target` (computed from g_host0/g_ticks0 in dolphin_hook.cpp)
// is directly comparable. `event_type` is an opaque pointer to Dolphin's EventType —
// we never dereference it here; the registered fire-callback (set by the fork) casts it
// back and invokes the real Dolphin callback so the BODY stays unchanged.
struct SbEvent {
    uint64_t due_ticks;
    uint64_t seq;        // FIFO tiebreak, mirrors CoreTiming's fifo_order semantics
    void*    event_type; // opaque CoreTiming::EventType*
    uint64_t userdata;
};

// Min-heap by (due_ticks, seq). std::*_heap with greater-ordering = a min-heap.
struct SbEventGreater {
    bool operator()(const SbEvent& a, const SbEvent& b) const {
        if (a.due_ticks != b.due_ticks) return a.due_ticks > b.due_ticks;
        return a.seq > b.seq;
    }
};

class SbTimeline {
public:
    // Schedule an owned event `delta` ticks past `now_ticks` (the fork passes GetTicks()
    // so absolute-due math is identical to CoreTiming's `GetTicks() + cycles_into_future`).
    void schedule(int64_t delta, void* event_type, uint64_t userdata, uint64_t now_ticks) {
        std::lock_guard<std::mutex> lk(m_lock);
        const int64_t due = (int64_t)now_ticks + delta;
        m_heap.push_back(SbEvent{(uint64_t)(due < 0 ? 0 : due), m_seq++, event_type, userdata});
        std::push_heap(m_heap.begin(), m_heap.end(), SbEventGreater{});
        m_now = now_ticks;
    }

    // Advance the timeline to `target_ticks` (host-clock target from the governor) in ONE
    // step: fire every due event with `late = now - due`, exactly mirroring
    // CoreTiming::Advance's cyclesLate so the AudioDMA self-reschedule
    // (`period - cycles_late`) keeps working unchanged. A fired callback may re-schedule
    // (re-enter schedule()) — the lock is dropped across the callback to allow that, and
    // the post-fire re-pop picks up the new entry, so a far jump re-fires the periodic
    // event until it catches up (no event skipping), same property the batched Advance
    // relies on.
    void advance_to(uint64_t target_ticks) {
        if (!m_fire_cb) { m_now = target_ticks; return; }   // seam not wired → inert
        for (;;) {
            SbEvent ev;
            {
                std::lock_guard<std::mutex> lk(m_lock);
                if (m_heap.empty() || m_heap.front().due_ticks > target_ticks) {
                    m_now = target_ticks;
                    break;
                }
                std::pop_heap(m_heap.begin(), m_heap.end(), SbEventGreater{});
                ev = m_heap.back();
                m_heap.pop_back();
                m_now = ev.due_ticks;   // time IS at the event when its callback runs
            }
            const int64_t late = (int64_t)target_ticks - (int64_t)ev.due_ticks;
            // Run Dolphin's AudioDMACallback body (UpdateAudioDMA + self-reschedule).
            // The callback re-enters schedule() with now=GetTicks(); GetTicks() is still
            // CoreTiming-owned (we only moved the audio event, not the tick source), so the
            // re-scheduled due stays coherent with the rest of the heap.
            m_fired++;
            m_fire_cb(ev.event_type, ev.userdata, late);
        }
        dbg_tick();
    }

    void set_fire_cb(void (*cb)(void*, uint64_t, int64_t)) { m_fire_cb = cb; }

private:
    void dbg_tick() {
        if (!timeline_dbg()) return;
        static long long last_print_fired = 0;
        static struct timespec t0 = [] {
            struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t;
        }();
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        if (t.tv_sec != t0.tv_sec) {
            fprintf(stderr, "[timeline] fired=%lld/s now=%llu heap=%zu\n",
                    (long long)(m_fired - last_print_fired),
                    (unsigned long long)m_now, m_heap.size());
            last_print_fired = m_fired;
            t0 = t;
        }
    }

    std::mutex          m_lock;
    std::vector<SbEvent> m_heap;
    uint64_t            m_seq   = 0;
    uint64_t            m_now   = 0;
    long long           m_fired = 0;
    void (*m_fire_cb)(void*, uint64_t, int64_t) = nullptr;
};

SbTimeline& timeline() {
    static SbTimeline t;
    return t;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// C ABI seam surface — the ONLY symbols the fork (and dolphin_hook.cpp) reference.
// Keeping them extern "C" means the fork can forward-declare them without pulling in
// any runtime headers, and they link against the standalone-compiled object.
// ─────────────────────────────────────────────────────────────────────────────
extern "C" {

// Query: is native-time ownership enabled? Fork seams (SystemTimers::Init,
// CoreTimingManager::ScheduleEvent) gate on this so flag-off is byte-for-byte the
// old path.
bool sb_own_time_enabled() { return own_time_enabled_impl(); }

// Fork → timeline: register the thunk that fires a Dolphin EventType. The thunk
// (defined in the fork next to CoreTimingManager) casts `event_type` back to
// CoreTiming::EventType* and calls its callback(system, userdata, late). Called once.
void sb_timeline_set_fire_cb(void (*cb)(void* event_type, uint64_t userdata, int64_t late)) {
    timeline().set_fire_cb(cb);
}

// Fork ScheduleEvent → timeline: forward an OWNED event instead of pushing the heap.
// `now_ticks` is the fork's GetTicks() at call time so due math matches CoreTiming.
void sb_timeline_schedule(int64_t cycles_into_future, void* event_type,
                          uint64_t userdata, uint64_t now_ticks) {
    if (!own_time_enabled_impl()) return;   // belt-and-suspenders; fork gates too
    timeline().schedule(cycles_into_future, event_type, userdata, now_ticks);
}

// Governor (dolphin_hook.cpp catch-up site) → timeline: fire all owned events due by
// the host-clock target. No-op (just records now) when the flag is off or no event
// has been scheduled, so the call is safe to leave in the governor unconditionally.
void sb_timeline_advance_to(uint64_t target_ticks) {
    if (!own_time_enabled_impl()) return;
    timeline().advance_to(target_ticks);
}

} // extern "C"

// ═════════════════════════════════════════════════════════════════════════════
// ==== REQUIRED SEAM ==== (fork edits — DOCUMENTED here, applied in the fork tree)
// ═════════════════════════════════════════════════════════════════════════════
// These are the exact edits to OUR Dolphin fork (externals/dolphin). They are NOT
// applied in this commit; this file links standalone with them absent (inert).
//
// ── SEAM 1: externals/.../Core/Core/CoreTiming.h, struct EventType ────────────
//   struct EventType
//   {
//     TimedCallback callback;
//     const std::string* name;
//     bool owned = false;          // + SUNBRIGHT: fired by SbTimeline, not the heap
//   };
//
// ── SEAM 2: externals/.../Core/Core/HW/SystemTimers.cpp, SystemTimersManager::Init
//   (right after line 271, where m_event_type_audio_dma is registered) ──────────
//   extern "C" bool sb_own_time_enabled();
//   if (sb_own_time_enabled())
//     m_event_type_audio_dma->owned = true;   // own ONLY the audio-DMA cadence
//
// ── SEAM 3: externals/.../Core/Core/CoreTiming.cpp, CoreTimingManager::ScheduleEvent
//   (inside the `if (from_cpu_thread)` branch, BEFORE the heap emplace at line ~284) ─
//   extern "C" void sb_timeline_schedule(int64_t, void*, uint64_t, uint64_t);
//   if (event_type->owned) {
//     // Forward to the host-clock timeline instead of the CoreTiming heap. GetTicks()
//     // here is the same base used to compute `timeout`, so due math is identical.
//     sb_timeline_schedule(cycles_into_future, event_type, userdata, GetTicks());
//     return;
//   }
//   // NOTE: only forward CPU-thread schedules. An owned event scheduled off-thread is
//   // not expected (AudioDMA self-reschedules from its own CPU-thread callback); leave
//   // the off-thread branch untouched. The initial schedule in SystemTimers::Init runs
//   // on the CPU thread, so the first AudioDMA event is captured here too.
//
// ── SEAM 4: externals/.../Core/Core/CoreTiming.cpp, CoreTimingManager::Init (end) ─
//   extern "C" void sb_timeline_set_fire_cb(void (*)(void*, uint64_t, int64_t));
//   sb_timeline_set_fire_cb([](void* et, uint64_t ud, int64_t late) {
//     auto* type = static_cast<CoreTiming::EventType*>(et);
//     type->callback(Core::System::GetInstance(), ud, late);  // unchanged body
//   });
//   // (Capture-less lambda → plain function pointer; uses the global System instance,
//   // matching how the governor already reaches CoreTiming.)
//
// ── SEAM 5: runtime/dolphin_hook.cpp, charge_guest_time() batched catch-up ────────
//   At the catch-up site (~line 412-423), AFTER computing the host-clock `target`
//   and BEFORE/ALONGSIDE the batched Advance, drive the owned timeline to the same
//   target so the AudioDMA event fires on our clock:
//     extern "C" void sb_timeline_advance_to(uint64_t target_ticks);
//     ...
//     const s64 target = (s64)g_ticks0 + (s64)(el * (double)tps);
//     sb_timeline_advance_to((uint64_t)target);   // + own the audio-DMA cadence
//     const s64 deficit = target - glob.global_timer;
//     ...
//   advance_to is a no-op when SUNBRIGHT_OWN_TIME is unset, so this line is safe to
//   leave unconditionally. Keep the EE-masked / pending-only IRQ discipline around it
//   exactly as the existing Advance (the AID raise is already owned natively, so the
//   fired AudioDMACallback's INT_AID just sets pending — delivered at the boundary).
// ═════════════════════════════════════════════════════════════════════════════
