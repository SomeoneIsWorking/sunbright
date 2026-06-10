// Native VI frame-sync — owns the 1/60s frame heartbeat (user-approved scope, handoff.md).
//
// The GC frame clock is standard Nintendo SDK VI code: __VIRetraceHandler (ISR @0x8034ED18,
// raised by the emulated VI hardware interrupt) bumps retraceCount, runs the pre/post-retrace
// callbacks, applies the VIFlush'd shadow registers to VI hardware, and wakes the threads
// sleeping in VIWaitForRetrace. Under the native scheduler that chain is fragile: the IRQ must
// be delivered at an interp boundary, the ISR runs under SingleStep on a borrowed context, and
// the render thread's wake raced into a wild read (ea=0x32323502) in TVideo::waitForRetrace.
//
// This override owns the whole retrace transaction natively on the WAITING thread instead:
// VIWaitForRetrace paces on Dolphin's host-side vi_end_field_event (the VI device still
// generates fields; we keep Dolphin for GPU/present), then performs the ISR's documented
// bookkeeping itself — count bump, pre-CB, shadow-register flush apply, SI refresh, post-CB,
// queue wakeup. The guest VI DI interrupt enables are cleared on every entry so the emulated
// IRQ does not double-run the original ISR. No HW interrupt, no OSSleepThread-on-retrace.
//
// Globals extracted from the SMS GMSE01 DOL (scratch/bin/ppcdis.py over __VIRetraceHandler):
//   retraceCount   0x8040E8D0 (r13-0x58f0)      flushFlag      0x8040E8D4
//   retraceQueue   0x8040E8D8                   preRetraceCB   0x8040E8E0
//   postRetraceCB  0x8040E8E4                   changedMask    0x8040E908/0x8040E90C (64-bit)
//   shadowRegs     0x80403340 (= VI BSS 0x804032C8 + 0x78), halfword per VI reg, MSB-first mask
//   SI sample regs 0x8040E910 <- [0x8040340C], 0x8040E914 <- [0x804033E0] (on flush apply)
// The real ISR latches the flush on a matching even/odd field (0x8040E900); we apply it on the
// next retrace unconditionally — same frame granularity, no field parity dependence.

#include "../overrides.h"
#include "../intrinsics.h"
#include "../dolphin_hook.h"
#include "Core/System.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/Fifo.h"
#include <chrono>
#include <thread>

#ifdef HAVE_DOLPHIN_CORE

namespace {

constexpr u32 RETRACE_COUNT = 0x8040E8D0u;
constexpr u32 FLUSH_FLAG    = 0x8040E8D4u;
constexpr u32 RETRACE_QUEUE = 0x8040E8D8u;
constexpr u32 PRE_CB        = 0x8040E8E0u;
constexpr u32 POST_CB       = 0x8040E8E4u;
constexpr u32 CHANGED_HI    = 0x8040E908u;
constexpr u32 CHANGED_LO    = 0x8040E90Cu;
constexpr u32 SHADOW_REGS   = 0x80403340u;
constexpr u32 VI_MMIO       = 0xCC002000u;
constexpr u32 OS_WAKEUP     = 0x803493CCu;  // OSWakeupThread (routed native via native_os)
constexpr u32 SI_REFRESH    = 0x80369ADCu;  // SIRefreshSamplingRate
constexpr u32 VI_WAIT       = 0x8034F684u;  // VIWaitForRetrace (guest)
constexpr u32 VI_GET_COUNT  = 0x803504ECu;  // VIGetRetraceCount (guest)


// Clear the INT_ENB bit (bit 28) of the four VI display-interrupt registers so the emulated VI
// never raises the retrace IRQ — the original __VIRetraceHandler must not run in parallel with
// this native port. VIConfigure rewrites these on a render-mode change; re-clearing on every
// VIWaitForRetrace entry re-owns them the next frame.
void disable_vi_interrupts() {
    for (u32 reg = 0xCC002030u; reg <= 0xCC00203Cu; reg += 4) {
        u32 v = sb_r32(reg);
        if (v & 0x10000000u) sb_w32(reg, v & ~0x10000000u);
    }
}

inline void guest_call(CPUState& cpu, u32 addr) {
    cpu.lr = VI_WAIT;        // valid code address for interp return detection if non-recomp
    call_ppc(cpu, addr);
}

// The ISR's flush-apply: write every changed shadow halfword to the VI hardware registers
// (this is how VISetNextFrameBuffer/VIFlush reach Dolphin's VI — the XFB swap), then clear the
// changed mask + flush flag, refresh the SI sampling registers and call SIRefreshSamplingRate.
void apply_flush(CPUState& cpu) {
    const u32 hi = sb_r32(CHANGED_HI), lo = sb_r32(CHANGED_LO);
    for (int i = 0; i < 32; i++)
        if (hi & (0x80000000u >> i)) sb_w16(VI_MMIO + i * 2, sb_r16(SHADOW_REGS + i * 2));
    for (int i = 0; i < 32; i++)
        if (lo & (0x80000000u >> i)) sb_w16(VI_MMIO + (32 + i) * 2, sb_r16(SHADOW_REGS + (32 + i) * 2));
    sb_w32(CHANGED_HI, 0);
    sb_w32(CHANGED_LO, 0);
    sb_w32(FLUSH_FLAG, 0);
    sb_w32(0x8040E910u, sb_r32(0x8040340Cu));
    sb_w32(0x8040E914u, sb_r32(0x804033E0u));
    guest_call(cpu, SI_REFRESH);
}

// One native retrace: the documented __VIRetraceHandler bookkeeping, minus the HW interrupt.
void retrace_tick(CPUState& cpu) {
    const u32 count = sb_r32(RETRACE_COUNT) + 1;
    sb_w32(RETRACE_COUNT, count);
    if (u32 cb = sb_r32(PRE_CB))  { cpu.gpr[3] = count; guest_call(cpu, cb); }
    if (sb_r32(FLUSH_FLAG) != 0)  apply_flush(cpu);
    if (u32 cb = sb_r32(POST_CB)) { cpu.gpr[3] = count; guest_call(cpu, cb); }
    cpu.gpr[3] = RETRACE_QUEUE;   // wake any guest thread still sleeping on the retrace queue
    guest_call(cpu, OS_WAKEUP);
}

// VIWaitForRetrace 0x8034F684 — the native frame heartbeat, NOT a wait on emulated time.
// Each call IS one frame: yield once so other ready guest threads run their per-frame work,
// perform the retrace transaction (count bump, callbacks, flush apply), and return. Boot
// progress is event-driven and deterministic — it never depends on the emulated VI clock
// crawling forward. Real-time pacing (when not turbo) is a HOST-clock sleep, never a guest
// time loop.
// Per-phase wall-time accounting for the frame heartbeat (printed every 64 frames; permanent
// perf diagnostic — found the 1.2s/frame mystery by naming the slow phase, 2026-06-10).
struct PhaseTimer {
    const char* name; long long* acc;
    std::chrono::steady_clock::time_point t0;
    PhaseTimer(const char* n, long long* a) : name(n), acc(a), t0(std::chrono::steady_clock::now()) {}
    ~PhaseTimer() { *acc += std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0).count(); }
};
static long long g_ph_pace, g_ph_field, g_ph_bp, g_ph_drain, g_ph_pump, g_ph_tick;
static long g_ph_frames;

SUNBRIGHT_OVERRIDE(ov_VIWaitForRetrace, VI_WAIT) {
    disable_vi_interrupts();
    if (++g_ph_frames % 64 == 0) {
        fprintf(stderr, "[vi-perf] %ld frames: pace=%lldms field=%lldms backpressure=%lldms "
                "drain=%lldms pump=%lldms tick=%lldms\n", g_ph_frames,
                g_ph_pace/1000, g_ph_field/1000, g_ph_bp/1000, g_ph_drain/1000,
                g_ph_pump/1000, g_ph_tick/1000);
        g_ph_pace=g_ph_field=g_ph_bp=g_ph_drain=g_ph_pump=g_ph_tick=0;
    }
    static const bool paced = !getenv("SUNBRIGHT_TURBO");
    if (paced) {
        PhaseTimer _t("pace", &g_ph_pace);
        // Host-clock 60 Hz frame pacing (fields are 1/59.94s; one retrace per call).
        using clock = std::chrono::steady_clock;
        static clock::time_point next = clock::now();
        const auto period = std::chrono::nanoseconds(16'683'350);
        std::this_thread::sleep_until(next);
        const auto now = clock::now();
        next = (now > next + period) ? now + period : next + period;
    }
    // Emu-clock half of the heartbeat: force one VI field of emulated time per host frame, so
    // Dolphin's VI presents fields at the heartbeat rate (emu speed ~1x in render loops) instead
    // of crawling at the per-call cycle-charge rate (~0.014x — frames every 1.2s, 2026-06-10).
    { PhaseTimer _t("field", &g_ph_field); sunbright_wait_vi_field(cpu); }
    // GPU backpressure — the half of the GC frame contract the host-clock pacing alone misses.
    // On hardware the CPU can never run far ahead of the GPU: the CP FIFO + draw-sync breakpoint
    // throttle it to ~2 frames. Our native heartbeat returned at a fixed 60 Hz regardless, so at
    // boot (first-use pipeline compilation makes the host GPU hitch for ~seconds) the game ran
    // 18+ frames ahead; Dolphin's PixelEngine coalesces draw-sync token interrupts (keeps only
    // the latest), the TDrawSyncManager thread lost tokens, the breakpoint stopped advancing,
    // and the pipeline deadlocked at the hi watermark (2026-06-10). Wait host-side until the
    // FIFO has drained to a sane depth before starting the next frame — exactly the stall real
    // hardware would impose, delivered as a host sleep instead of a guest spin.
    {
        PhaseTimer _t("bp", &g_ph_bp);
        auto& sys  = Core::System::GetInstance();
        auto& fifo = sys.GetCommandProcessor().GetFifo();
        const u32 fifo_cap = fifo.CPEnd.load() - fifo.CPBase.load();
        if (fifo_cap > 0x2000u) {
            // Threshold cap/8: tight on purpose. A watermark-relative threshold (hiwm*3/4) was
            // tried and DEADLOCKED again — letting the queue grow re-enters the token-coalescing
            // /suspension regime (Dolphin PE keeps only the latest token). Until token delivery
            // is provably loss-free at depth, keep the queue shallow. [[no-bandaids: the slow
            // serial cycle is the GPU-side drain rate, being root-caused separately.]]
            const u32 threshold = fifo_cap / 8;
            int spins = 0;
            while (fifo.CPReadWriteDistance.load() > threshold && spins++ < 2500) {
                sys.GetFifo().RunGpu();                    // CP hardware never sleeps with data pending
                // The drain needs the WHOLE pipeline serviced, not just the GPU thread: the PE
                // draw-sync token lands as a CoreTiming event (needs Advance), its interrupt as a
                // native dispatch, and the TDrawSyncManager thread (which moves the breakpoint)
                // needs the nthr token. A bare host sleep here starved all three (2026-06-10).
                sunbright_poll_yield();                    // Advance + native IRQ dispatch (EE-safe)
                nthrt_yield_current(&cpu);                 // let the woken sync thread run NOW
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            static long warned = 0;
            if (spins >= 2500 && warned++ < 8)
                fprintf(stderr, "[vi] GPU backpressure timeout: dist=%08x after 500ms (GPU stalled?)\n",
                        fifo.CPReadWriteDistance.load());
        }
    }
    // Frame barrier: give the rest of the frame to EVERY other runnable thread — including
    // lower-priority ones (the boot setup thread at prio 0x11 vs main's 16), which a plain
    // priority yield would starve forever. On the GC the retrace wait blocked the caller, so
    // all runnable work proceeded during the frame; block_drain is the deterministic
    // equivalent: resume exactly when everyone else has run to its own block/yield point.
    { PhaseTimer _t("drain", &g_ph_drain); nthrt_block_drain(&cpu); }
    // The heartbeat is also the device-IRQ pump: deliver pending device completions (DSP, SI,
    // any residual DVD interrupt) once per frame, deterministically.
    { PhaseTimer _t("pump", &g_ph_pump); sunbright_poll_yield(); }
    { PhaseTimer _t("tick", &g_ph_tick); retrace_tick(cpu); }
}

// VIGetRetraceCount 0x803504EC — plain load of the (natively bumped) counter.
SUNBRIGHT_OVERRIDE(ov_VIGetRetraceCount, VI_GET_COUNT) {
    cpu.gpr[3] = sb_r32(RETRACE_COUNT);
}

}  // namespace

// Idle-driver retrace: on real hardware the VI retrace is an INTERRUPT, independent of any
// thread. The native port runs the retrace transaction inside VIWaitForRetrace on the calling
// thread — correct while the render loop is alive, but when the GX pusher is SUSPENDED at the
// CP hi watermark (FIFO pacing), nobody calls VIWaitForRetrace and the vsync callbacks that
// move the FIFO breakpoint never run → GPU pinned at the breakpoint → full deadlock
// (2026-06-10). When every guest thread is blocked, the idle driver calls this once per
// presented VI field to run the same retrace transaction from the idle context.
bool sunbright_vi_idle_retrace(CPUState& cpu) {
    extern unsigned long long watchdog_vi_fields();
    static unsigned long long last_fields = 0;
    const unsigned long long f = watchdog_vi_fields();
    if (f == last_fields) return false;
    last_fields = f;
    retrace_tick(cpu);
    return true;
}

#else
bool sunbright_vi_idle_retrace(CPUState&) { return false; }
#endif  // HAVE_DOLPHIN_CORE
