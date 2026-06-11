#include "dolphin_hook.h"
#include "memory_bridge.h"
#include "intrinsics.h"
#include "overrides.h"
#include "native_threads.h"
#include "native_os.h"
#include "probe_server.h"
#include "sb_spin.h"
#include "sb_assert.h"
#include "watchdog.h"
#include <dlfcn.h>
#include <mutex>
#include <atomic>
#include <cstdlib>
#ifdef HAVE_DOLPHIN_CORE
#  include "Core/HW/SystemTimers.h"
#  include "Core/PowerPC/Interpreter/Interpreter.h"
#endif
#include <cstdio>
#include <cstring>
#include <csetjmp>
#include <chrono>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <execinfo.h>
#include <sys/resource.h>
#include <unistd.h>
#ifdef HAVE_DOLPHIN_CORE
#  include "Core/System.h"
#  include "Core/Core.h"
#  include "Core/CoreTiming.h"
#  include "Core/HW/VideoInterface.h"
#  include "Core/HW/ProcessorInterface.h"
#  include "Core/HLE/HLE.h"
#  include "VideoCommon/Fifo.h"
#  include "VideoCommon/CommandProcessor.h"
#  include "Core/PowerPC/MMU.h"
#  include "Core/PowerPC/PPCSymbolDB.h"
#endif

extern void mem_w32(u32 ea, u32 v);   // from memory_bridge
extern u16  mem_r16(u32 ea);
extern void mem_w16(u32 ea, u16 v);

using RecompFunc = void (*)(CPUState&);
struct JumpEntry { uint32_t addr; RecompFunc fn; };

// The recompiled function table is linked directly into the binary (no dlopen).
extern "C" const JumpEntry g_recomp_table[];
extern "C" const size_t    g_recomp_table_size;

// Direct-mapped dispatch table: PPC instructions are 4-byte aligned, so we index
// a flat array by (addr - base) >> 2. This is the hot path — call_ppc consults it
// on EVERY bl and EVERY return — so it must be a single array load, not a hashmap.
// Without this populated, recomp_lookup always missed, and every intra-recomp call
// fell back to copying the whole register file into Dolphin and bouncing to the JIT
// (then back in via the trampoline) — a full state copy per call/return.
static RecompFunc* g_dispatch = nullptr;
static u32 g_dispatch_lo = 0, g_dispatch_hi = 0;   // [lo, hi) PPC address range
// Set only when overrides / forced-JIT ranges are actually registered, so the
// common case skips those lookups entirely.
static bool g_have_overrides  = false;
static bool g_have_jit_forced = false;

void recomp_build_dispatch() {
    if (g_dispatch || g_recomp_table_size == 0) return;
    u32 lo = 0xFFFFFFFFu, hi = 0;
    for (size_t i = 0; i < g_recomp_table_size; i++) {
        u32 a = g_recomp_table[i].addr;
        lo = std::min(lo, a);
        hi = std::max(hi, a + 4);
    }
    g_dispatch_lo = lo; g_dispatch_hi = hi;
    const size_t n = (hi - lo) >> 2;
    g_dispatch = new RecompFunc[n]();              // zero-initialised
    for (size_t i = 0; i < g_recomp_table_size; i++)
        g_dispatch[(g_recomp_table[i].addr - lo) >> 2] = g_recomp_table[i].fn;

    g_have_overrides  = overrides_registered();
    g_have_jit_forced = jit_forced_registered();
    native_os_init();   // register the native OS primitive set (consulted below)
    fprintf(stderr, "[sunbright] Dispatch table: %zu funcs over [%08x,%08x) (%zu slots)\n",
            g_recomp_table_size, lo, hi, n);
}

bool dolphin_hook_install(const char*) { recomp_build_dispatch(); return true; }
void dolphin_hook_uninstall() { delete[] g_dispatch; g_dispatch = nullptr; }

RecompFunc recomp_lookup(u32 address) {
    // Forced-JIT and hand-written overrides are rare; skip both lookups entirely
    // unless something was actually registered.
    if (g_have_jit_forced && is_jit_forced(address)) return nullptr;
    if (g_have_overrides) { if (RecompFunc ov = override_lookup(address)) return ov; }
    // Hot path: single array load.
    if (address >= g_dispatch_lo && address < g_dispatch_hi && !(address & 3))
        return g_dispatch[(address - g_dispatch_lo) >> 2];
    return nullptr;
}

// Super-call: the ORIGINAL generated body for `address`, bypassing any override registered on it.
// This is what lets an override observe/adjust a function and still run the real thing — the basis
// of the "own the object model, keep Dolphin's GPU" render port (e.g. hook J2DScreen::draw, fix the
// 2D layout, then run the original draw). Returns nullptr if `address` isn't recompiled.
RecompFunc recomp_raw(u32 address) {
    if (address >= g_dispatch_lo && address < g_dispatch_hi && !(address & 3))
        return g_dispatch[(address - g_dispatch_lo) >> 2];
    return nullptr;
}

// Observe a recompiled function without replacing it: SUNBRIGHT_WATCH=<hexaddr>
// logs args (and, for a matrix loader, the 3x4 matrix at r3) every time that
// address is called. This is the capture primitive the motion interpolator will
// use — point it at J3DModel::viewCalc / a draw fn to grab per-object transforms.
extern f32 mem_rf32(u32 ea);   // from memory_bridge
extern u32 mem_r32(u32 ea);    // from memory_bridge
static u32 watch_addr() {
    static const u32 a = getenv("SUNBRIGHT_WATCH")
                         ? (u32)strtoul(getenv("SUNBRIGHT_WATCH"), nullptr, 16) : 0;
    return a;
}

// SUNBRIGHT_OSWATCH: pure observation of the OS blocking/sync primitives (GMSE01),
// to map "what blocks waiting on what, woken by whom" during e.g. audio init. Logs the
// call's object (r3 = thread queue / mutex / cond) and the current OSThread* (low-mem
// slot 0x800000E4, as OSGetCurrentThread reads it). Observation only — never alters
// control flow; called from both the recomp path (call_ppc) and the interpreter loop
// (run_jit_sync) so it sees the calls regardless of which backend runs them.
void sunbright_trace_interp_pc(u32 pc, u32 r3, u32 lr);   // defined at file end (inote tracer)
void sunbright_trace_jit_entry(u32 address, u32 r3, u32 lr);  // defined at file end (jnote tracer)
static void os_sync_watch(u32 pc, u32 r3, u32 r4, u32 lr) {
    static const bool on = getenv("SUNBRIGHT_OSWATCH") != nullptr;
    if (!on || pc < 0x80346710u || pc > 0x803493ccu) return;   // fast range filter
    const char* name = nullptr;
    switch (pc) {
        case 0x80346710u: name = "OSLockMutex";    break;
        case 0x803467ecu: name = "OSUnlockMutex";  break;
        case 0x80346a00u: name = "OSWaitCond";     break;
        case 0x80346ad4u: name = "OSSignalCond";   break;
        case 0x80348d08u: name = "OSJoinThread";   break;
        case 0x803492e0u: name = "OSSleepThread";  break;
        case 0x803493ccu: name = "OSWakeupThread"; break;
        default: return;
    }
    fprintf(stderr, "[oswatch] %-14s cur=%08x r3=%08x r4=%08x (lr=%08x)\n",
            name, mem_r32(0x800000E4u), r3, r4, lr);
}

#ifdef HAVE_DOLPHIN_CORE
bool interp_run_until(u32 ret, long budget, u32 sp_floor = 0);   // defined below; used by call_ppc

// ── Per-target interpreter-step profiler (SUNBRIGHT_INTERP_PROFILE=1) ────────────
// The project directive: never lean on the interpreter for game logic — recompile or
// port it. To know WHICH non-recomp targets cost the most interpreter time, the
// interpreter loop records the steps it took for its most recent run in
// g_last_interp_steps; call_ppc attributes that to the target address. A periodic +
// atexit dump prints the worst offenders (the run currently ends in an abort, so we
// also dump from a timer-less periodic flush keyed on accumulated steps). All gated.
static const bool g_interp_profile = getenv("SUNBRIGHT_INTERP_PROFILE") != nullptr;
static long g_last_interp_steps = 0;   // steps the last interp_run_until took

// Spin-locator: when interp_run_until blows its step budget, we want to know WHERE it was
// spinning (the OS idle loop? a busy-wait? a specific function?). Sample the pc periodically into
// a histogram and, on the FATAL, dump the hottest PCs — that names the loop the scheduler is stuck
// in. Sampling is a cheap masked-counter add, only the histogram insert is occasional.
static std::unordered_map<u32, unsigned long long> g_interp_pc_hist;
static void sunbright_dump_pc_hist(const char* tag) {
    if (g_interp_pc_hist.empty()) return;
    std::vector<std::pair<u32, unsigned long long>> v(g_interp_pc_hist.begin(), g_interp_pc_hist.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    fprintf(stderr, "  [%s] hottest interpreted PCs (sampled):\n", tag);
    for (size_t i = 0; i < v.size() && i < 16; i++)
        fprintf(stderr, "    pc=%08x  %llu samples\n", v[i].first, v[i].second);
}
struct InterpProfEntry { unsigned long long steps = 0; unsigned long long calls = 0; };
static std::unordered_map<u32, InterpProfEntry>& interp_prof_map() {
    static std::unordered_map<u32, InterpProfEntry> m;
    return m;
}
void sunbright_dump_interp_profile() {
    if (!g_interp_profile) return;
    auto& m = interp_prof_map();
    std::vector<std::pair<u32, InterpProfEntry>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(),
              [](auto& a, auto& b) { return a.second.steps > b.second.steps; });
    fprintf(stderr, "\n[interp-profile] top interpreter-step consumers (addr  steps  calls):\n");
    unsigned long long total = 0;
    for (auto& e : v) total += e.second.steps;
    int shown = 0;
    for (auto& e : v) {
        if (shown++ >= 40) break;
        fprintf(stderr, "[interp-profile] 0x%08x  %14llu  %10llu  (%.1f%%)\n",
                e.first, e.second.steps, e.second.calls,
                total ? 100.0 * (double)e.second.steps / (double)total : 0.0);
    }
    fprintf(stderr, "[interp-profile] total interpreter steps across %zu targets: %llu\n",
            v.size(), total);
    fflush(stderr);
}

// ── Dispatch profiler (SUNBRIGHT_DISPATCH_PROFILE) ───────────────────────────
// Histograms the guest block addresses dispatched through the JIT→recomp boundary
// (SunbrightBridge::Run). A spin/idle loop that bounces out to Dolphin's CPU loop every
// iteration — instead of staying on the native C stack — shows up as the dominant entry.
// This is what the same-address poll detector (memory_bridge.cpp) misses for a multi-address
// wait loop: it never advances CoreTiming, so the loop crawls at a fraction of real-time.
static const bool g_dispatch_profile = getenv("SUNBRIGHT_DISPATCH_PROFILE") != nullptr;
static std::unordered_map<u32, unsigned long long>& dispatch_prof_map() {
    static std::unordered_map<u32, unsigned long long> m; return m;
}
void sunbright_dump_dispatch_profile() {
    if (!g_dispatch_profile) return;
    auto& m = dispatch_prof_map();
    std::vector<std::pair<u32, unsigned long long>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    unsigned long long total = 0;
    for (auto& e : v) total += e.second;
    fprintf(stderr, "\n[dispatch-profile] top JIT->recomp dispatch entries (addr  count):\n");
    int shown = 0;
    for (auto& e : v) {
        if (shown++ >= 40) break;
        fprintf(stderr, "[dispatch-profile] 0x%08x  %14llu  (%.1f%%)\n", e.first, e.second,
                total ? 100.0 * (double)e.second / (double)total : 0.0);
    }
    fprintf(stderr, "[dispatch-profile] total dispatches across %zu entries: %llu\n",
            v.size(), total);
    fflush(stderr);
}
void sunbright_dispatch_profile_note(u32 pc) {
    if (!g_dispatch_profile) return;
    static bool registered = (atexit(sunbright_dump_dispatch_profile), true);
    (void)registered;
    dispatch_prof_map()[pc]++;
    static unsigned long long since = 0;
    if ((++since & 0x3FFFFFull) == 0) sunbright_dump_dispatch_profile();   // ~every 4M
}

// ── Tail-branch handoff ──────────────────────────────────────────────────────
// Set by SunbrightBridge::Run for the duration of a top-level recomp entry so a tail-branch (or a
// context switch, see call_ppc) into non-recomp code can siglongjmp back out, abandoning the
// recomp C-call stack and letting Dolphin's CPU loop take over.
static thread_local sigjmp_buf* g_tail_jmp = nullptr;
sigjmp_buf* sunbright_set_tail_jmp(sigjmp_buf* j) {
    sigjmp_buf* prev = g_tail_jmp; g_tail_jmp = j; return prev;
}

// GC OS context-switch primitive OSLoadContext (0x80343fe4, JIT-only): it rfi's into another
// thread's context and never returns to its caller. Running it synchronously under the interpreter
// (run_jit_sync) tries to run the whole switched-to thread inline → it reschedules to the OS idle
// loop and spins. Instead, hand off to Dolphin's CPU loop (which already runs the GC threading
// correctly — the hybrid design): commit state and longjmp out, exactly like a tail branch. The
// recomp re-enters via the JIT trampoline when a recompiled function next runs.
static constexpr u32 OS_LOAD_CONTEXT = 0x80343fe4u;

// Fail-fast on a branch/call through a NULL (or near-NULL) guest pointer — i.e. a `bctrl`/`bctr`/
// `bl` whose target is 0 or low garbage. This is the ORIGINATOR of the "ISI exception at
// 0x00000000" + downstream run_jit_sync step-budget abort: some struct's function pointer / vtable
// slot was clobbered to 0 (usually by earlier memory corruption), then called. Trapping here dumps
// the recomp call chain at the bad call instead of limping into Dolphin's exception path. Valid
// code lives at >= 0x80003100; anything below 0x80001000 is wild.
[[noreturn]] static void sb_fatal_wild_branch(u32 target, const CPUState& cpu) {
    fflush(stdout);
    fprintf(stderr,
        "\n[sunbright] FATAL branch through wild/NULL pointer: target=0x%08x (lr=%08x ctr=%08x)\n"
        "  A bl/bctr/bctrl jumped to a non-code address — a clobbered function pointer or vtable\n"
        "  slot called as a function. This is the ISI-at-0 originator; trapping at the call site.\n"
        "  Native backtrace (recomp call chain, innermost first):\n",
        target, cpu.lr, cpu.ctr);
    void* bt[96];
    int bn = backtrace(bt, 96);
    backtrace_symbols_fd(bt, bn, fileno(stderr));
    fflush(stderr);
    struct rlimit no_core{0, 0};
    setrlimit(RLIMIT_CORE, &no_core);
    abort();
}
// Wild iff the high bit isn't set: a real guest code address is always in 0x8xxxxxxx (cached) or
// 0xCxxxxxxx (uncached mirror). A target < 0x80000000 is NULL or a pointer that lost its top
// bit(s) (e.g. a virtual→physical rlwinm mask whose rfi must run under Dolphin's MMU, not recomp).
// This range also can't false-positive on the exception vectors (0x80000100+ — high bit set).
static inline bool sb_is_wild_branch_target(u32 a) { return a < 0x80000000u; }
#endif

#ifdef HAVE_DOLPHIN_CORE
// Recomp guest-time advancement — the recomp equivalent of the JIT's per-block downcount.
// Pure-recomp execution previously advanced NO guest time: TB (SystemTimers::GetFakeTimeBase)
// derives from CoreTiming ticks, so any recomp loop waiting on time (the TCardManager EXI
// insertion debounce; any OSGetTime timeout) spun forever and starved every scheduled device
// event (VI fields stop → frame-wait threads never wake). Charge a per-call cycle cost against
// PowerPC downcount and run CoreTiming::Advance when the slice expires — device events (VI/DSP/
// DVD/throttle) fire, interrupts become *pending* only (no CheckExceptions; delivery stays at
// the boundaries — see the interrupt-delivery hazard in CLAUDE.md). Guarded: only the declared
// CPU thread (token holder) may advance, and never reentrantly (device callbacks can call back
// into guest helpers).
static constexpr int kCyclesPerCall = 96;   // ~avg recomp function cost; order-of-magnitude is enough
static thread_local bool t_in_advance = false;
static inline void charge_guest_time() {
    // Armed only once the GC OS has threading + exception handlers installed (current-thread
    // pointer set). Charging during early boot shifts device/decrementer events into the
    // real-mode, vectors-not-yet-installed init window → wild vectoring (run12, 2026-06-09).
    static bool armed = false;
    if (!armed) {
        u32 cur = 0;
        if (u8* p = sb_ram_fast(0x800000E4u)) { memcpy(&cur, p, 4); cur = __builtin_bswap32(cur); }
        if (!cur) return;
        armed = true;
    }
    auto& sys = Core::System::GetInstance();
    auto& ppc = sys.GetPPCState();
    ppc.downcount -= kCyclesPerCall;
    if (ppc.downcount <= 0 && !t_in_advance && Core::IsCPUThread()) {
        t_in_advance = true;
        // Mask EE around Advance: it ends in CheckExternalExceptions(), which would DELIVER the
        // interrupt right here (pc=vector, MSR→real mode) on the mid-tree global ppc with nobody
        // to run the ISR — leaving every later MMIO bridge access untranslated (the cc006800
        // "Unable to resolve" storm, run13). Pending IRQs stay pending; the idle driver and the
        // interp paths deliver them at a proper boundary. (Same pattern as sb_poll_fire's MMIO arm.)
        const u32 saved_msr = ppc.msr.Hex;
        ppc.msr.Hex &= ~0x8000u;
        sys.GetCoreTiming().Advance();
        ppc.msr.Hex = saved_msr;
        t_in_advance = false;
        // Deliver any pending external IRQ HERE, at the recomp call boundary — natively (see
        // native_dispatch_one). On hardware the CP/VI/DSP interrupt preempts the running thread
        // immediately; recomp code that never blocks (a GX display-list push loop) otherwise
        // outruns the GPU for a whole frame and overflows the CP FIFO before reaching any other
        // delivery point ("FIFO is overflowed by GatherPipe! CPU thread is too fast"). The
        // handler is seeded from the LIVE recomp context (real r1/r2/r13 — it runs on the
        // interrupted thread's stack, exactly like the GC exception prologue); the caller's own
        // CPUState is untouched (handlers preserve non-volatiles per the EABI).
        extern bool sunbright_deliver_pending_recomp(u32 logical_msr);
        if (saved_msr & 0x8000u) sunbright_deliver_pending_recomp(saved_msr);
    }
}
#endif

extern "C" void sb_trace(const char* tag, u32 a, u32 b, u32 c, u32 d);   // probe /tracelog ring

void call_ppc(CPUState& cpu, u32 address) {
#ifdef HAVE_DOLPHIN_CORE
    if (sb_is_wild_branch_target(address)) sb_fatal_wild_branch(address, cpu);
    charge_guest_time();
#endif
    // SUNBRIGHT_HUDCALLS: log each DISTINCT function called during the in-game HUD draw (g_in_hud),
    // to find the indirect element-draw functions (coins/water gauge/lives) that aren't perform's
    // direct calls. Deduped so the log is the set of HUD-involved functions, not every call.
    extern bool g_in_hud;
    static const bool hudcalls = getenv("SUNBRIGHT_HUDCALLS") != nullptr;
    if (hudcalls && g_in_hud) {
        static std::unordered_map<u32, char> seen;
        if (seen.find(address) == seen.end()) {
            seen[address] = 1;
            fprintf(stderr, "[hudcall] %08x  r3=%08x\n", address, cpu.gpr[3]);
        }
    }
    os_sync_watch(address, cpu.gpr[3], cpu.gpr[4], cpu.lr);
    // SUNBRIGHT_DBG_CARD: ring-trace every call into the CARD/EXI SDK region (incl. the unnamed
    // __CARDTxHandler callback chain) — the CARDMount lost-completion deadlock localizer.
    static const bool dbg_card = getenv("SUNBRIGHT_DBG_CARD") != nullptr;
    if (dbg_card && address >= 0x80354000u && address < 0x8036b000u)
        sb_trace("card", address, cpu.gpr[3], cpu.gpr[4], cpu.lr);
    // SUNBRIGHT_DBG_AUD: the audio frame-cycle chain (dead-audio bug) — __AIDHandler →
    // syncAudio → OSSendMessage(audioproc_mq) → audio thread updateDac.
    static const bool dbg_aud = getenv("SUNBRIGHT_DBG_AUD") != nullptr;
    if (dbg_aud && (address == 0x80352ae4u || address == 0x803110f0u ||
                    address == 0x80346190u || address == 0x80346508u))
        sb_trace("aud", address, cpu.gpr[3], cpu.gpr[4], cpu.lr);
    // SUNBRIGHT_DBG_WAVE: the wave-bank load chain — direct stderr (sparse events; the ring
    // floods and drops them between probe polls).
    static const bool dbg_wave = getenv("SUNBRIGHT_DBG_WAVE") != nullptr;
    if (dbg_wave && address == 0x80301850u) {   // checkSceneWaveOnMemory: log args AND result
        const u32 a3 = cpu.gpr[3], a4 = cpu.gpr[4];
        RecompFunc f2 = recomp_lookup(address);
        if (f2) {
            f2(cpu);
            fprintf(stderr, "[wave] checkSceneWaveOnMemory(%u,%u) -> %d\n", a3, a4, (int)cpu.gpr[3]);
            return;
        }
    }
    if (dbg_wave && (address == 0x80318050u || address == 0x80310694u ||
                     address == 0x80310994u || address == 0x80301884u ||
                     address == 0x803017b0u || address == 0x80015640u ||
                     address == 0x8001569cu || address == 0x8030dc7cu /*BankMgr::noteOn*/ ||
                     address == 0x8031c894u /*TTrack::stopSeq*/ || address == 0x803068d8u /*JAIBasic::stopSeq*/ ||
                     address == 0x8031bb08u /*TTrack::mainProc*/ ||
                     address == 0x80016978u /*MSBgm::startBGM — the title-music start funnel*/ ||
                     address == 0x8031deb4u /*TrackMgr::allocNewRoot*/ ||
                     address == 0x8031dcdcu /*TrackMgr::handleToSeq*/ ||
                     address == 0x8031df48u /*TrackMgr::registTrack*/ ||
                     address == 0x8031c818u /*TTrack::startSeq*/ ||
                     address == 0x80301e80u /*JAIBasic::startSoundActor*/ ||
                     address == 0x80301fc4u /*startSoundDirectID*/ ||
                     address == 0x80302034u /*startSoundIndirectID*/ ||
                     address == 0x80306a1cu /*checkEntriedSeq*/ ||
                     address == 0x80307e18u /*checkStartedSeq*/ ||
                     address == 0x80307facu /*checkReadSeq*/ ||
                     address == 0x8030d284u /*trackToSeqp*/))
        fprintf(stderr, "[wave] call %08x r3=%08x r4=%08x lr=%08x\n",
                address, cpu.gpr[3], cpu.gpr[4], cpu.lr);
    // SUNBRIGHT_DBG_NOTE: JAS note/voice lifecycle (the choppy-music bug) — noteOn, noteOff,
    // release/force-stop, DSP queue add/remove. d = monotonic ms (note-duration measurement).
    static const bool dbg_note = getenv("SUNBRIGHT_DBG_NOTE") != nullptr;
    if (dbg_note && (address == 0x8030dc7cu /*BankMgr::noteOn*/ ||
                     address == 0x8031c894u /*TTrack::stopSeq*/ || address == 0x803068d8u /*JAIBasic::stopSeq*/ ||
                     address == 0x8031ab50u /*TTrack::noteOff*/ ||
                     address == 0x80312790u /*TChannel::releaseOsc*/ ||
                     address == 0x8031273cu /*TChannel::forceStopOsc*/ ||
                     address == 0x80314660u /*TDSPChannel::forceStop*/ ||
                     address == 0x80311550u /*DSPQueue::enQueue*/ ||
                     address == 0x80311708u /*DSPQueue::deleteQueue*/ ||
                     address == 0x8031c914u /*TTrack::closeTrack*/ ||
                     address == 0x8031ce68u /*TTrack::openTrack*/  ||
                     address == 0x8031defcu /*TrackMgr::deAllocRoot*/ ||
                     address == 0x8031deb4u /*TrackMgr::allocNewRoot — song start*/ ||
                     address == 0x8031dd00u /*TrackMgr::reset*/ ||
                     address == 0x8031c894u /*TTrack::stopSeq*/ ||
                     address == 0x8031c818u /*TTrack::startSeq*/ ||
                     address == 0x803068d8u /*JAIBasic::stopSeq*/ ||
                     address == 0x803017b0u /*JAIBasic::loadSceneWave*/ ||
                     address == 0x80301850u /*JAIBasic::checkSceneWaveOnMemory*/ ||
                     address == 0x80301884u /*JAIBasic::loadGroupWave*/ ||
                     address == 0x80310994u /*WaveBankMgr::loadWave*/ ||
                     address == 0x80310694u /*WaveArcLoader::loadWave*/ ||
                     address == 0x80015640u /*MSound::loadWave(scene) — scene-code callers*/ ||
                     address == 0x8001569cu /*MSound::loadGroupWave*/ ||
                     address == 0x802bc10cu /*orphan overlap copy (diag)*/ ||
                     address == 0x802bb920u /*scene sound-init — REAL entry (loadWave caller)*/ ||
                     address == 0x802b76f4u || address == 0x802b77ecu ||
                     address == 0x802b77fcu || address == 0x802b7898u ||
                     address == 0x80299838u /*static caller of 802b76f4*/ ||
                     address == 0x80346190u /*OSSendMessage (JAS dvd queue chase)*/ ||
                     address == 0x80318050u /*wave-stream chunk continuation (ARQ cb)*/ ||
                     address == 0x80346258u /*OSReceiveMessage*/ ||
                     address == 0x80348d08u /*OSJoinThread — join-once semantics chase*/ ||
                     address == 0x80348374u /*OSIsThreadTerminated*/)) {
        // c = caller lr (who ends the note/track), d = monotonic ms.
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        sb_trace("note", address, cpu.gpr[3], cpu.lr,
                 (u32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000));
    }
    if (address == watch_addr() && watch_addr() != 0) {
        static unsigned long n = 0;
        if ((n++ % 1000) == 0) {
            u32 mtx = cpu.gpr[3];
            fprintf(stderr, "[watch] %08x call#%lu r3=%08x r4=%08x", address, n, mtx, cpu.gpr[4]);
            if (mtx >= 0x80000000u && mtx < 0x81800000u)
                fprintf(stderr, "  pos=(%.2f, %.2f, %.2f)",
                        mem_rf32(mtx + 12), mem_rf32(mtx + 28), mem_rf32(mtx + 44));
            fprintf(stderr, "\n");
        }
    }
    // Native OS primitive takes precedence over the recomp body / interpreter (genuinely
    // native, no super-call): run it and return to the caller, exactly like the callee's blr.
    if (NativeOSFn nf = native_os_lookup(address)) {
        if (g_probe_enabled) g_probe.call_native_os.fetch_add(1, std::memory_order_relaxed);
        nf(cpu); return;
    }
    RecompFunc fn = recomp_lookup(address);
    if (fn) {
        // Recompiled target → nested native call. Control comes back here when the
        // callee returns (its blr is a C return); the caller then continues inline.
        if (g_probe_enabled) g_probe.call_recomp.fetch_add(1, std::memory_order_relaxed);
        // SUNBRIGHT_DBG_SPCHK: SP-imbalance detector. The PPC ABI guarantees r1 (SP) is identical
        // before and after any call — the callee tears down exactly the frame it built. So a recomp
        // call that RETURNS with a changed SP (and did not hand off via a context switch) has an
        // unbalanced-stack mistranslation in its body or call tree. This names the innermost culprit
        // directly (it fires at the deepest call where SP first diverges), instead of force_jit
        // bisecting level by level. Pinned the boot endRendering→waitForRetrace r31 clobber.
        static const bool dbg_spchk = getenv("SUNBRIGHT_DBG_SPCHK") != nullptr;
        if (dbg_spchk) {
            const u32 sp_before = cpu.gpr[1];
            const bool cs_before = g_recomp_context_switched;
            fn(cpu);
            if (!cs_before && !g_recomp_context_switched && cpu.gpr[1] != sp_before) {
                static long hits = 0;
                if (hits++ < 32)
                    fprintf(stderr, "[spchk] call %08x returned SP %08x -> %08x (delta %+d) — "
                            "unbalanced stack in its recomp tree\n",
                            address, sp_before, cpu.gpr[1], (int)(cpu.gpr[1] - sp_before));
            }
            return;
        }
        fn(cpu);
        return;
    }
    if (g_probe_enabled) g_probe.call_interp.fetch_add(1, std::memory_order_relaxed);
#ifdef HAVE_DOLPHIN_CORE
    static const bool trace = getenv("SUNBRIGHT_TRACE") != nullptr;
    if (trace) {
        static u32 last_non_recomp = 0;
        if (address != last_non_recomp) {
            fprintf(stderr, "[call_ppc] non-recomp call to 0x%08x\n", address);
            last_non_recomp = address;
        }
    }
    auto& ppc = Core::System::GetInstance().GetPPCState();
    // Non-recomp callee: run it to completion under the interpreter and return to the
    // C caller. We stop when control returns to cpu.lr (the address the caller put in
    // LR for a `bl`, or the caller's own return target for a tail branch) — that is
    // precisely the callee's final blr. Running it synchronously keeps the rest of
    // the caller on the native C stack.
    const u32 ret = cpu.lr;
    // Stack floor for return detection: a real return to `ret` lands with the stack unwound back
    // to the caller's SP. This disambiguates a recursive interpreted callee that transiently
    // revisits `ret` at a deeper frame (see interp_run_until / the __construct_array crash).
    const u32 sp_floor = cpu.gpr[1];
    cpu_to_dolphin_state(cpu, ppc);
    ppc.pc = ppc.npc = address;
    // A context switch never returns to us — hand off to Dolphin's CPU loop instead of spinning.
    if (address == OS_LOAD_CONTEXT && g_tail_jmp) {
        if (getenv("SUNBRIGHT_DBG_CTX")) {
            u32 ctxp = cpu.gpr[3];
            fprintf(stderr, "[ctx] OSLoadContext ctx=%08x loaded r1=%08x srr0=%08x lr=%08x\n",
                    ctxp, mem_r32(ctxp + 4), mem_r32(ctxp + 0x198), mem_r32(ctxp + 0x84));
        }
        g_recomp_context_switched = true; siglongjmp(*g_tail_jmp, 1);
    }
    constexpr long MAX = 500'000'000;
    if (!interp_run_until(ret, MAX, sp_floor)) {
        // The interpreter never returned to the caller's LR within the budget — the
        // callee rescheduled to a never-returning context (the GC OS idle loop after a
        // blocking OS call) and is spinning. This is THE root cause behind the later
        // "wild guest write": bailing here and continuing with half-executed, inconsistent
        // state corrupts a base/stack pointer downstream. Fail fast AT the cause instead of
        // limping on — same fail-fast discipline as the wild-write trap. (Native threading
        // removes this budget entirely: a blocked guest thread parks on its own fiber rather
        // than spinning the interpreter.)
        fflush(stdout);
        fprintf(stderr,
            "\n[sunbright] FATAL run_jit_sync(%08x→%08x) exceeded step budget (%ld steps)\n"
            "  The interpreted callee never returned to LR — it rescheduled to a\n"
            "  never-returning context (blocking OS call → OS idle loop). Continuing would\n"
            "  corrupt state. This is the audio-init/blocking stall native threading fixes.\n"
            "  Native backtrace (recomp call chain, innermost first):\n",
            address, ret, MAX);
        sunbright_dump_pc_hist("spin");
        void* bt[96];
        int bn = backtrace(bt, 96);
        backtrace_symbols_fd(bt, bn, fileno(stderr));
        fflush(stderr);
        // Suppress the core dump: with the default core_pattern piping to systemd-coredump,
        // SIGABRT would dump this multi-GB (Dolphin + game) process and wedge for minutes —
        // looking like "prints FATAL but never exits". The backtrace above is the artifact we
        // want; skip the core so abort() terminates promptly (exit 134).
        struct rlimit no_core{0, 0};
        setrlimit(RLIMIT_CORE, &no_core);
        abort();
    }
    dolphin_state_to_cpu(ppc, cpu);
    // SUNBRIGHT_DBG_SPCHK (interp path): same SP-balance invariant as the recomp path above. A
    // non-recomp callee run under interp_run_until must return with r1 unchanged; if it comes back
    // with a different SP, interp_run_until stopped at the wrong point (pc==ret but the stack not
    // unwound to the caller's SP — the sp_floor `>=` test accepts sp = sp_floor + k>0). That leaks a
    // wrong SP back into the recomp caller, whose epilogue then loads its saved non-volatiles from
    // the wrong slots (the boot endRendering→waitForRetrace r31 clobber: SP came back +8).
    {
        static const bool dbg_spchk = getenv("SUNBRIGHT_DBG_SPCHK") != nullptr;
        if (dbg_spchk && !g_recomp_context_switched && cpu.gpr[1] != sp_floor) {
            static long hits = 0;
            if (hits++ < 32)
                fprintf(stderr, "[spchk/interp] call %08x (ret=%08x) returned SP %08x -> %08x "
                        "(delta %+d) — interp_run_until stopped with stack not unwound to caller SP\n",
                        address, ret, sp_floor, cpu.gpr[1], (int)(cpu.gpr[1] - sp_floor));
        }
    }
    if (g_interp_profile) {
        // Attribute the steps this interpreted callee took to its entry address. call_ppc is
        // effectively single-threaded for guest code (the CPU token serializes it), so a plain
        // map needs no lock. Register the atexit dump once; also flush periodically because the
        // boot run currently ends in abort() (wild-read trap) which still runs atexit handlers,
        // but the periodic dump guarantees we have data even if a future abort path skips them.
        static bool registered = (atexit(sunbright_dump_interp_profile), true);
        (void)registered;
        auto& e = interp_prof_map()[address];
        e.steps += (unsigned long long)g_last_interp_steps;
        e.calls += 1;
        static unsigned long long since_dump = 0;
        since_dump += (unsigned long long)g_last_interp_steps;
        if (since_dump >= 50'000'000ull) { since_dump = 0; sunbright_dump_interp_profile(); }
    }
#else
    fprintf(stderr, "[sunbright] call_ppc 0x%08x: not recompiled and no JIT available\n", address);
#endif
}

#ifdef HAVE_DOLPHIN_CORE
// Step the Dolphin interpreter from ppc.pc until it reaches `ret`, dispatching native OS
// primitives reached along the way (so e.g. OSSleepThread parks the host thread instead of
// being single-stepped). `budget` caps the steps (0 = unlimited, for a guest thread body that
// blocks via native parking rather than returning). Returns true if `ret` was reached, false if
// the budget was exhausted. SingleStep (not SingleStepInner) advances CoreTiming and checks
// exceptions each instruction, so HW-wait/poll loops make progress and their interrupts fire.
// Run the interpreter until control returns to `ret`. `sp_floor` (when non-zero) makes the
// return detection STACK-AWARE: a genuine return to `ret` only happens once the guest stack has
// unwound back to the caller's stack pointer (ppc.gpr[1] >= sp_floor). Without it, a bare
// `pc == ret` match is ambiguous — if the interpreted callee re-enters that same address (e.g. a
// RECURSIVE callee whose own post-call continuation IS `ret`, like __construct_array 0x80337f78),
// we would stop EARLY, mid-call, with a half-unwound register file (the TBeamManager-ctor crash:
// the callee's loop r30/r29/r31 leaked back to the caller as `this`≈5). At a true return the
// callee has balanced its frame so ppc.gpr[1] == sp_floor; nested re-entries sit at a deeper
// (smaller) SP, so `>= sp_floor` rejects them. Callers that aren't bl/bctrl returns (ISR/syscall/
// idle/thread-body) pass sp_floor=0 and keep the original bare-PC behavior.
// Per-step PC ring (single writer: the CPU-token holder). Dumped by the MEM-dispatch trap to show
// the exact instruction path that led into the impossible handler — cheaper and more precise than
// re-running with trace env vars (REPL-era diagnostic, 2026-06-10).
static constexpr int kPcRingN = 4096;           // power of two
static u32 g_pc_ring[kPcRingN];
static unsigned char g_pc_ring_tid[kPcRingN];
static unsigned g_pc_ring_i = 0;
// PC bits 1:0 are always 0 — stash a 2-bit host-thread tag there so the dump exposes interleaving:
// the interpreter state (global ppc) is single-writer BY CONTRACT (the CPU token); two tags
// alternating inside one "call chain" = two host threads racing the interpreter (the corruption
// class behind the spurious MEM dispatch / mid-run teleports, 2026-06-10).
static std::atomic<int> g_tid_seq{0};
static inline unsigned char sb_ring_tag() {
    static thread_local int t_tag = g_tid_seq.fetch_add(1, std::memory_order_relaxed);
    return (unsigned char)t_tag;
}
static inline void sb_ring_push(u32 pc) {
    const unsigned i = g_pc_ring_i++ & (kPcRingN - 1);
    g_pc_ring[i] = pc; g_pc_ring_tid[i] = sb_ring_tag();
}
// Dump the ring as collapsed straight-line runs: "start..end" per run, one run per branch taken.
// 4096 raw steps collapse to ~a screen of control flow — enough to see the whole call chain.
static void sunbright_dump_pc_ring(FILE* f) {
    fprintf(f, "  Last %d interp steps as straight-line runs (oldest first):\n", kPcRingN);
    u32 run_start = 0, prev = 0; unsigned tag = 0; bool open = false; int col = 0;
    for (int i = 0; i < kPcRingN; i++) {
        const unsigned idx = (g_pc_ring_i + (unsigned)i) & (kPcRingN - 1);
        const u32 p = g_pc_ring[idx]; const unsigned t = g_pc_ring_tid[idx];
        if (!p) continue;
        if (!open) { run_start = prev = p; tag = t; open = true; continue; }
        if (p == prev + 4 && t == tag) { prev = p; continue; }
        fprintf(f, "%s%08x..%08x/%u", (col % 4) ? "  " : "    ", run_start, prev, tag);
        if ((++col % 4) == 0) fputc('\n', f);
        run_start = prev = p; tag = t;
    }
    if (open) fprintf(f, "%s%08x..%08x/%u\n", (col % 4) ? "  " : "    ", run_start, prev, tag);
}

// Interpreter token guard: only the nthr token holder (or pre-adoption boot / the idle-hook
// context) may step the shared interpreter. A violator scrambles the global ppc — the corruption
// class behind the spurious MEM dispatch. Suspended interp frames (a thread parked mid-intercept)
// are legal; the invariant is on who is STEPPING now.
static int native_dispatch_pending(const CPUState* seed = nullptr);   // fwd (native dispatcher below)
static bool in_native_dispatch();        // fwd

bool interp_run_until(u32 ret, long budget, u32 sp_floor) {
    if (!nthr::self_may_run_guest()) {
        fprintf(stderr, "\n[interp] TOKEN VIOLATION: this host thread entered the interpreter "
                "without holding the nthr token (pc=%08x ret=%08x).\n",
                Core::System::GetInstance().GetPPCState().pc, ret);
        sunbright_park("interpreter token violation");
    }
    auto& ppc    = Core::System::GetInstance().GetPPCState();
    auto& interp = Core::System::GetInstance().GetInterpreter();
    struct InterpTimer {
        std::chrono::steady_clock::time_point t0;
        InterpTimer() { if (g_probe_enabled) t0 = std::chrono::steady_clock::now(); }
        ~InterpTimer() {
            if (g_probe_enabled)
                g_probe.interp_ns.fetch_add(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - t0).count(),
                    std::memory_order_relaxed);
        }
    } _it;
    long n = 0;
    u32 prev_pc = ppc.pc;   // the instruction executed just before the current pc (the branch, if pc went wild)
    // SUNBRIGHT_DBG_OVERSHOOT: catch the sp_floor return-detection OVERSHOOT — the interpreter reaches
    // the caller's return address `ret` but sp hasn't unwound to sp_floor, so the loop keeps stepping
    // INTO the caller's body (running recomp-owned code under interp with a possibly diverged register
    // file). Logs the moment a legit-looking return is rejected, with sp vs sp_floor + r31, capped.
    static const bool dbg_overshoot = getenv("SUNBRIGHT_DBG_OVERSHOOT") != nullptr;
    while (ppc.pc != ret || (sp_floor && ppc.gpr[1] < sp_floor)) {
        if (dbg_overshoot && ppc.pc == ret && sp_floor && ppc.gpr[1] < sp_floor) {
            static long hits = 0;
            if (hits++ < 64)
                fprintf(stderr, "[overshoot] reached ret=%08x but sp=%08x < sp_floor=%08x "
                        "(r31=%08x r3=%08x lr=%08x) — NOT stopping, stepping into caller body (step %ld)\n",
                        ret, ppc.gpr[1], sp_floor, ppc.gpr[31], ppc.gpr[3], ppc.spr[SPR_LR], n);
        }
        // Wild-branch invariant on the INTERP path (mirror of sb_fatal_wild_branch on the recomp
        // path): guest code lives at >= 0x80003100, so a pc in low/NULL space means a branch went
        // through a clobbered/NULL function pointer. The next interpreter fetch would raise Dolphin's
        // "ISI exception at 0x0", then the OS exception machinery (JUTException reporter daemon) spins
        // forever under us → the 500M-step budget abort. Trap AT the branch instead, naming prev_pc
        // (the offending bl/bctr/blr) + LR/CTR so the originating call site is visible.
        //   Exceptions to the rule (legitimately low pc): (1) the real-mode PowerPC exception-vector
        //   page 0x100..0x1800 — an `sc`/external-interrupt/DSI taken mid-step vectors here and the
        //   handler `rfi`s back; (2) pc==ret, a sentinel caller may pass a low return address.
        const bool in_vector_page = ppc.pc >= 0x00000100u && ppc.pc < 0x00001800u;
        if (!(ppc.pc >= 0x80001000u || ppc.pc == ret || in_vector_page)) {
            fprintf(stderr,
                "\n[interp] wild branch to pc=%08x from prev_pc=%08x: lr=%08x ctr=%08x r1=%08x "
                "r3=%08x (ret=%08x, %ld steps in). A guest bl/bctr/blr went through a NULL/wild "
                "pointer — or the CODE at prev_pc was overwritten (verify with /r).\n",
                ppc.pc, prev_pc, ppc.spr[SPR_LR], ppc.spr[SPR_CTR], ppc.gpr[1], ppc.gpr[3], ret, n);
            sunbright_dump_pc_ring(stderr);
            // Park (not abort): the guest state — especially possibly-overwritten CODE bytes — is
            // the evidence; the REPL (/r) must be able to read it post-mortem.
            sunbright_park("interp wild branch");
        }
        if (budget) { if (n++ >= budget) { g_last_interp_steps = n; return false; } }
        else if ((++n & 0x7FFFFFF) == 0) { // budget-less (thread body): periodic progress probe
            fprintf(stderr, "[interp] thread-body still running pc=%08x after %ldM steps\n",
                    ppc.pc, n / 1'000'000);
            // A thread body that interprets >400M steps without blocking/exiting is a livelock
            // (e.g. the JUTException reporter spinning its crash screen). Always die loudly with
            // a dump instead of pegging the host forever — the freeze must kill, never linger.
            if (n >= 400'000'000) {
                fprintf(stderr,
                    "\n[interp] FATAL: thread-body livelock — %ldM interpreter steps without "
                    "exit/block.\n  pc=%08x lr=%08x r1=%08x r3=%08x cur_thread=%08x ret=%08x\n"
                    "  Top interp PCs (spin-locator samples):\n",
                    n / 1'000'000, ppc.pc, ppc.spr[SPR_LR], ppc.gpr[1], ppc.gpr[3],
                    mem_r32(0x800000E4u), ret);
                {
                    std::vector<std::pair<u32, unsigned long long>> top(g_interp_pc_hist.begin(), g_interp_pc_hist.end());
                    std::sort(top.begin(), top.end(),
                              [](auto& a, auto& b) { return a.second > b.second; });
                    for (size_t i = 0; i < top.size() && i < 12; i++)
                        fprintf(stderr, "    %08x  %llu samples\n", top[i].first,
                                (unsigned long long)top[i].second);
                }
                sunbright_park("interp thread-body livelock");
            }
        }
        if (NativeOSFn nf = native_os_lookup(ppc.pc)) {
            static bool first = true;
            if (first) { first = false;
                fprintf(stderr, "[native_os] first interpreter-path intercept at %08x\n", ppc.pc); }
            static const bool dbg_isr = getenv("SUNBRIGHT_DBG_ISR") != nullptr;
            if (dbg_isr) {
                static long hits = 0;
                if (hits++ < 256)
                    fprintf(stderr, "[isr-intercept] pc=%08x prev_pc=%08x lr=%08x r3=%08x r1=%08x ret=%08x\n",
                            ppc.pc, prev_pc, ppc.spr[SPR_LR], ppc.gpr[3], ppc.gpr[1], ret);
            }
            CPUState t; dolphin_state_to_cpu(ppc, t); t.pc = ppc.pc;
            nf(t);
            cpu_to_dolphin_state(t, ppc);
            ppc.pc = ppc.npc = t.lr;   // return to the guest caller (LR), like the callee's blr
            continue;
        }
        // SUNBRIGHT_DBG_DISPATCH: log every __OSDispatchInterrupt entry (0x80345d84) with Dolphin's
        // live PI cause/mask and the MI protection-cause reg — names the IRQ source the GC OS is
        // about to dispatch. Built to pin the spurious MEM-interrupt → OSError 15 → JUTException
        // reporter livelock (2026-06-10). Permanent diagnostic (see memory keep-diagnostics).
        static const bool dbg_dispatch = getenv("SUNBRIGHT_DBG_DISPATCH") != nullptr;
        if (dbg_dispatch && ppc.pc == 0x80345d84u) {
            auto& pi = Core::System::GetInstance().GetProcessorInterface();
            static long hits = 0;
            if (hits++ < 512)
                fprintf(stderr, "[dispatch] PI cause=%08x mask=%08x mem_cause=%04x srr0=%08x "
                        "intmsk_c4=%08x_%08x\n",
                        pi.GetCause(), pi.GetMask(), mem_r16(0xCC00401Eu), ppc.spr[SPR_SRR0],
                        mem_r32(0x800000C4u), mem_r32(0x800000C8u));
        }
        // MEM-protection interrupt trap (always on): Dolphin NEVER raises PI cause bit 0x80 and the
        // MI cause register is never set, so the GC OS reaching MEMIntrruptHandler (0x80346370 —
        // raises OSError 15 → JUTException crash screen → MarErrException readPad livelock) is
        // impossible on faithful hardware state. If we get here, OUR runtime corrupted the dispatch
        // (stale/garbage INTSR or MI reads). Dump the dispatch registers + live PI state and PARK so
        // the REPL (/r, /stack, /cur) can inspect the exact faulting state. 2026-06-10.
        if (ppc.pc == 0x80346370u) {
            auto& pi = Core::System::GetInstance().GetProcessorInterface();
            fprintf(stderr,
                "\n[interp] TRAP: MEMIntrruptHandler dispatched (impossible on faithful HW state).\n"
                "  PI cause=%08x mask=%08x  MI cause reg=%04x\n"
                "  srr0=%08x srr1=%08x lr=%08x ctx(r4)=%08x intr#(r29)=%08x\n"
                "  intmsk C4=%08x C8=%08x  cur_thread=%08x\n"
                "  Parked for REPL inspection (kill -9 to exit).\n",
                pi.GetCause(), pi.GetMask(), mem_r16(0xCC00401Eu),
                ppc.spr[SPR_SRR0], ppc.spr[SPR_SRR1], ppc.spr[SPR_LR], ppc.gpr[4], ppc.gpr[29],
                mem_r32(0x800000C4u), mem_r32(0x800000C8u), mem_r32(0x800000E4u));
            sunbright_dump_pc_ring(stderr);
            sunbright_park("spurious MEM-interrupt dispatch");
        }
        // DSP-handler step diag (temporary, surgical): print fetched opcode + HLE hook state at
        // the exact derail site 803378a8/803378ac (straight-line code cannot jump; either the
        // fetch differs from RAM or an HLE hook fires).
        if ((ppc.pc >= 0x80337880u && ppc.pc < 0x80337900u) && ppc.gpr[3] == 0x803e9700u) {
            auto& sysd = Core::System::GetInstance();
            const u32 op  = sysd.GetMMU().Read_Opcode(ppc.pc);
            const u32 ram = mem_r32(ppc.pc);
            fprintf(stderr, "[dspdiag-BAD] pc=%08x fetched=%08x ram=%08x lr=%08x r1=%08x r3=%08x\n",
                    ppc.pc, op, ram, ppc.spr[SPR_LR], ppc.gpr[1], ppc.gpr[3]);
            sunbright_dump_pc_ring(stderr);
            sunbright_park(op != ram ? "stale icache fetch" : "bad-context handler entry (fetch==ram)");
        }
        if (ppc.pc == 0x803378a8u || ppc.pc == 0x803378acu) {
            static long hits = 0;
            if (hits++ < 16) {
                auto& sysd = Core::System::GetInstance();
                const u32 op = sysd.GetMMU().Read_Opcode(ppc.pc);
                const u32 hook = HLE::GetHookByFunctionAddress(sysd.GetPPCSymbolDB(), ppc.pc);
                fprintf(stderr, "[dspdiag] pc=%08x fetched=%08x ram=%08x hle_hook=%u lr=%08x npc=%08x\n",
                        ppc.pc, op, mem_r32(ppc.pc), hook, ppc.spr[SPR_LR], ppc.npc);
            }
        }
        // Poisoned-entry trap: a function entered with LR == its own address returns to itself —
        // the __DSPHandler self-loop signature. Catch it AT entry so the ring shows the poisoner.
        if ((ppc.pc == 0x80337880u || ppc.pc == 0x80352ae4u || ppc.pc == 0x80353018u) &&
            ppc.spr[SPR_LR] == ppc.pc) {
            fprintf(stderr, "\n[interp] TRAP: handler %08x entered with LR == its own address "
                    "(r1=%08x ctr=%08x ret=%08x)\n", ppc.pc, ppc.gpr[1], ppc.spr[SPR_CTR], ret);
            sunbright_dump_pc_ring(stderr);
            sunbright_park("poisoned handler entry (LR == pc)");
        }
        os_sync_watch(ppc.pc, ppc.gpr[3], ppc.gpr[4], ppc.spr[SPR_LR]);
        // Interpreter-context twin of the DBG_NOTE tracers (third execution context — call_ppc
        // and the bridge JIT entry don't see interpreted code; tag "inote").
        sunbright_trace_interp_pc(ppc.pc, ppc.gpr[3], ppc.spr[SPR_LR]);
        if ((n & 0xFFFF) == 0) g_interp_pc_hist[ppc.pc]++;   // spin-locator sample (~every 64K steps)
        // Native external-interrupt delivery (completes the PC-native port of the GC interrupt
        // path): when an external IRQ is pending and the guest's EE is on, dispatch it natively
        // HERE — never let SingleStep vector through 0x80000500 into the guest
        // ExternalInterruptHandler/__OSDispatchInterrupt/OSLoadContext machinery (retired; its
        // re-entrancy under the hybrid produced the OSError-15 corruption class). sc/DSI/program
        // exceptions still vector normally — they are synchronous and self-contained.
        if ((ppc.Exceptions & 0x00000004u /*EXCEPTION_EXTERNAL_INT*/) && (ppc.msr.Hex & 0x8000u) &&
            !in_native_dispatch()) {
            // Full context save/restore around the dispatch — exactly what the GC exception
            // prologue/OSLoadContext pair does. The handlers run on the SAME global ppc (call_ppc
            // interp path), so without this the interrupted body resumed with the HANDLER's
            // register file (boot static-ctor loop continued with r3=garbage → the 0x10000000
            // wild write at 802c9f00, 2026-06-10).
            CPUState saved_ctx;
            dolphin_state_to_cpu(ppc, saved_ctx);
            const u32 r_pc = ppc.pc, r_npc = ppc.npc, r_msr = ppc.msr.Hex;
            const u32 r_srr0 = ppc.spr[SPR_SRR0], r_srr1 = ppc.spr[SPR_SRR1];
            native_dispatch_pending();
            cpu_to_dolphin_state(saved_ctx, ppc);
            ppc.pc = r_pc; ppc.npc = r_npc; ppc.msr.Hex = r_msr;
            ppc.spr[SPR_SRR0] = r_srr0; ppc.spr[SPR_SRR1] = r_srr1;
            continue;
        }
        // SUNBRIGHT_DBG_RAWTRACE=ADDR: once an interp run with ret==idle-sentinel reaches ADDR,
        // print EVERY step (pc lr r1 r3) until the run ends — the exact, uncollapsed flow.
        static const char* rawtrace_env = getenv("SUNBRIGHT_DBG_RAWTRACE");
        static const u32 rawtrace_at = rawtrace_env ? (u32)strtoul(rawtrace_env, nullptr, 16) : 0;
        static long rawtrace_left = 0;
        if (rawtrace_at && ppc.pc == rawtrace_at && ret == 0x80002FF8u) rawtrace_left = 1200;
        if (rawtrace_left > 0) {
            rawtrace_left--;
            fprintf(stderr, "[raw] %08x lr=%08x r1=%08x r3=%08x r4=%08x\n",
                    ppc.pc, ppc.spr[SPR_LR], ppc.gpr[1], ppc.gpr[3], ppc.gpr[4]);
        }
        prev_pc = ppc.pc;
        sb_ring_push(ppc.pc);
        interp.SingleStep();
        if (g_probe_enabled) g_probe.interp_steps.fetch_add(1, std::memory_order_relaxed);
    }
    g_last_interp_steps = n;
    return true;
}

// Called when a recomp RAM poll loop is detected (e.g. the GC OS idle loop spinning on
// RunQueueBits, which an ISR sets when a thread becomes runnable). The recomp runs the spin as a
// tight native loop, so CoreTiming never advances and no interrupt is ever delivered — the polled
// flag is never set. Advance emulated time; if interrupts are enabled and one becomes pending,
// Advance vectors to the handler — run the ISR (so e.g. a DVD/DSP/VI ISR calls OSWakeupThread,
// setting RunQueueBits) until it rfi's back, then the recomp's next read sees the updated flag.
// A guest `b .` (0x48000000) to park the idle PC at — a delivered IRQ needs a valid srr0 to rfi to.
// We never execute it (interp_run_until stops the moment pc returns there), so any one will do.
static u32 sunbright_idle_spin_pc() {
    // PLANT the spin — do not scavenge one from game text. The old scan (0x80003100..0x80040000)
    // found no 0x48000000 in SMS's init text (OS halt loops live at 0x8034xxxx, above the cap) and
    // fell back to 0x80003100 = real code, so the "idle spin" actually EXECUTED the program forward
    // on whatever stale registers ppc held — wandering into GXDrawDone's OSSleepThread call and
    // corrupting its wait queue (the 0x32323502 wild read, 2026-06-09). 0x80002FF8 sits in the
    // unused low-mem gap between the exception vectors (end 0x80001800) and the DOL text (0x80003100).
    static u32 cached = 0;
    if (cached) return cached;
    cached = 0x80002FF8u;
    mem_w32(cached, 0x48000000u);   // b .
    return cached;
}
// ─── Native __OSDispatchInterrupt — PC-native port of OSInterrupt.c (doldecomp/sms) ────────────
// The GC external-interrupt path (vector page 0x500 → ExternalInterruptHandler context save on the
// interrupt stack → __OSDispatchInterrupt MMIO cause walk → handler → __OSReschedule →
// OSLoadContext rfi) is single-threaded, non-reentrant guest machinery. Stepping it under the
// interpreter while our runtime owns threading/idle repeatedly produced impossible states (nested
// dispatch, scrambled interrupt index → spurious MEMIntrruptHandler → OSError 15 → JUTException
// crash screen). This is the behavior port: read Dolphin's live PI cause/mask host-side, build the
// OS cause word exactly as OSInterrupt.c does (DSP/AI/EXI sub-cause MMIO reads through the bridge),
// apply the OS mask globals, walk InterruptPrioTable, and CALL the registered guest handler
// directly (r3=interrupt, r4=current OSContext) via call_ppc — most handlers are recompiled. No
// vector page, no interrupt stack, no rfi, no nesting. Scheduling glue (OSDisableScheduler /
// __OSReschedule / OSLoadContext) is owned by nthr and intentionally absent.
//
// GMSE01 OS globals (from the dispatch disassembly at 0x80345d84, r13 = 0x804141C0):
extern "C" void sb_trace(const char* tag, u32 a, u32 b, u32 c, u32 d);   // probe /tracelog ring
static constexpr u32 OS_INTR_TABLE_PTR = 0x8040E7B0u;  // __OSInterruptHandlerTable → 0x80003040
static constexpr u32 OS_LAST_INTR      = 0x8040E7B8u;  // u16 __OSLastInterrupt
static constexpr u32 OS_LAST_SRR0      = 0x8040E7B4u;  // u32 __OSLastInterruptSrr0
static constexpr u32 OS_LAST_TIME      = 0x8040E7C0u;  // u64 __OSLastInterruptTime
static constexpr u32 kMask = 0x80000000u;               // OS_INTERRUPTMASK(n) = kMask >> n
// InterruptPrioTable, verbatim from OSInterrupt.c.
static const u32 kIntrPrio[] = {
    (kMask >> 23),                                   // PI_ERROR
    (kMask >> 25),                                   // PI_DEBUG
    0xF8000000u,                                     // MEM (0..4)
    (kMask >> 22),                                   // PI_RSW
    (kMask >> 24),                                   // PI_VI
    (kMask >> 18) | (kMask >> 19),                   // PI_PE (TOKEN|FINISH)
    (kMask >> 26),                                   // PI_HSP
    (kMask >> 6) | (kMask >> 7) | (kMask >> 8)       // DSP_ARAM | DSP_DSP | AI
        | 0x007F8000u                                // EXI (9..16)
        | (kMask >> 20) | (kMask >> 21),             // PI_SI | PI_DI
    (kMask >> 5),                                    // DSP_AI
    (kMask >> 17),                                   // PI_CP
    0xFFFFFFFFu,
};

// Dispatch ONE pending interrupt natively. Returns true if a handler ran (call again — more cause
// bits may remain), false when nothing is dispatchable (no PI cause, PI-masked, or OS-masked).
static thread_local bool t_in_native_dispatch = false;  // a handler's own bridge reads can poll-fire
static bool in_native_dispatch() { return t_in_native_dispatch; }
// Which DSP-CSR status bit the currently-dispatching handler is entitled to ack (0 outside a
// DSP-source dispatch). The memory bridge consults this to stop guest CSR write-backs from
// write-1-clearing a PENDING, UNDISPATCHED sibling status: on hardware an EE-on context can
// never observe a pending DSPINT (it vectors immediately), so the JASystem CPU→DSP mailbox kick
// (CSR |= 0x2 via read-modify-write) can never swallow a DSP-done there — here delivery is
// deferred to boundaries, and exactly that swallow killed all audio ~3 s into boot (intr7 frozen
// at 605, the kernel waiting forever on a DSP-done that was acked-by-accident; /tracelog #8469:
// dispatch read CSR=0x9D2 with 0x80 pending, next event the guest wrote 0x9D2 back).
static thread_local u32 t_dsp_ack_allowed = 0;
u32 sunbright_dsp_ack_allowed() { return t_dsp_ack_allowed; }
unsigned long g_nintr_counts[32];                        // per-interrupt dispatch counters (diag; probe /nintr)
unsigned long g_ds_token_dispatches = 0;                 // drawsync diag (probe /drawsync)
static bool native_dispatch_one(const CPUState* seed) {
    if (t_in_native_dispatch) return false;
    auto& sys = Core::System::GetInstance();
    auto& pi  = sys.GetProcessorInterface();
    const u32 intsr = pi.GetCause() & ~0x00010000u;   // drop the RSWST status bit, like the OS
    const u32 intmsk = pi.GetMask();
    if (intsr == 0 || (intsr & intmsk) == 0) return false;

    u32 cause = 0;
    // intsr & 0x80 (MEM) deliberately unhandled: Dolphin never raises it (see the MEM trap above).
    if (intsr & 0x40) {                                // DSP
        const u16 r = mem_r16(0xCC00500Au);            // __DSPRegs[5] (DSP CSR)
        sb_trace("dspint", intsr, r, 0, 0);            // CSR at dispatch (dead-audio localizer)
        if (r & 0x08) cause |= kMask >> 5;             // DSP_AI
        if (r & 0x20) cause |= kMask >> 6;             // DSP_ARAM
        if (r & 0x80) cause |= kMask >> 7;             // DSP_DSP
    }
    if (intsr & 0x20) {                                // AI
        const u32 r = mem_r32(0xCC006C00u);            // __AIRegs[0]
        if (r & 0x08) cause |= kMask >> 8;             // AI_AI
    }
    if (intsr & 0x10) {                                // EXI
        u32 r = mem_r32(0xCC006800u);                  // __EXIRegs[0]
        sb_trace("exint", intsr, r, mem_r32(0x800000C4u), mem_r32(0x800000C8u));
        if (r & 0x002) cause |= kMask >> 9;
        if (r & 0x008) cause |= kMask >> 10;
        if (r & 0x800) cause |= kMask >> 11;
        r = mem_r32(0xCC006814u);                      // __EXIRegs[5]
        if (r & 0x002) cause |= kMask >> 12;
        if (r & 0x008) cause |= kMask >> 13;
        if (r & 0x800) cause |= kMask >> 14;
        r = mem_r32(0xCC006828u);                      // __EXIRegs[10]
        if (r & 0x002) cause |= kMask >> 15;
        if (r & 0x008) cause |= kMask >> 16;
    }
    if (intsr & 0x2000) cause |= kMask >> 26;          // PI_HSP
    if (intsr & 0x1000) cause |= kMask >> 25;          // PI_DEBUG
    if (intsr & 0x0400) cause |= kMask >> 19;          // PI_PE_FINISH
    if (intsr & 0x0200) cause |= kMask >> 18;          // PI_PE_TOKEN
    if (intsr & 0x0100) cause |= kMask >> 24;          // PI_VI
    if (intsr & 0x0008) cause |= kMask >> 20;          // PI_SI
    if (intsr & 0x0004) cause |= kMask >> 21;          // PI_DI
    if (intsr & 0x0002) cause |= kMask >> 22;          // PI_RSW
    if (intsr & 0x0800) cause |= kMask >> 17;          // PI_CP
    if (intsr & 0x0001) cause |= kMask >> 23;          // PI_ERROR

    const u32 unmasked = cause & ~(mem_r32(0x800000C4u) | mem_r32(0x800000C8u));
    if (!unmasked) return false;                       // OS-masked: leave it pending, like the OS

    int interrupt = -1;
    for (const u32* p = kIntrPrio;; ++p)
        if (unmasked & *p) { interrupt = __builtin_clz(unmasked & *p); break; }

    const u32 table   = mem_r32(OS_INTR_TABLE_PTR);
    const u32 handler = table ? mem_r32(table + 4u * (u32)interrupt) : 0;
    if (!handler) {
        // Faithful OS behavior is to resume and re-take the interrupt; with no handler that is an
        // IRQ storm. An unmasked cause with no registered handler is a runtime bug — surface it.
        static long warned = 0;
        if (warned++ < 8)
            fprintf(stderr, "[nintr] unmasked interrupt %d (cause=%08x intsr=%08x) has no handler\n",
                    interrupt, unmasked, intsr);
        return false;
    }

    // PE_TOKEN (18): owned natively, loss-free. Dolphin's SetToken coalesces back-to-back tokens
    // (only the latest survives its single in-flight event); SMS ends every frame with two
    // (pollution-range value then frame-done 0), so one was dropped nearly every frame and the
    // drawsync pipeline crawled on the idle-driver's synthetic recovery (see pe_token_wrap.cpp).
    // The --wrap on SetToken recorded every interrupt-worthy value in order; deliver them all:
    // this is a native port of the GX token ISR 8035dd5c — call the registered GX token callback
    // (TokenCB @ 0x8040EA18, = TDrawSyncManager::drawSyncCallback) with r3 = token, then ack the
    // PE token interrupt via the ctrl reg (|0x4, exactly what the guest ISR writes). The guest
    // ISR itself must NOT also run — it would re-deliver the coalesced register value.
    if (interrupt == 18) {
        extern bool sb_token_ring_pop(uint16_t*);
        auto& tppc = sys.GetPPCState();
        uint16_t tok;
        int delivered = 0;
        while (sb_token_ring_pop(&tok)) {
            if (const u32 cb = mem_r32(0x8040EA18u)) {
                CPUState tcpu;
                if (seed) tcpu = *seed;
                else      dolphin_state_to_cpu(tppc, tcpu);
                tcpu.gpr[3] = tok;
                tcpu.lr     = sunbright_idle_spin_pc();
                t_in_native_dispatch = true;
                const u32 saved_msr = tppc.msr.Hex;
                tppc.msr.Hex &= ~0x8000u;              // ISR semantics: EE off in the handler
                call_ppc(tcpu, cb);
                tppc.msr.Hex = saved_msr;
                t_in_native_dispatch = false;
            }
            delivered++;
        }
        // Ack even when the ring was already drained (the coalesced event re-raised the signal
        // for a token we delivered earlier) — consuming it silently is what keeps the guest ISR
        // from double-running the latest value.
        mem_w16(0xCC00100Au, (u16)(mem_r16(0xCC00100Au) | 0x4u));
        g_nintr_counts[18]++;
        g_ds_token_dispatches += delivered;
        return true;
    }

    auto& ppc = sys.GetPPCState();
    if (interrupt > 4) {                               // the OS records only non-MEM dispatches
        mem_w16(OS_LAST_INTR, (u16)interrupt);
        mem_w32(OS_LAST_SRR0, ppc.pc);
        const u64 tb = Core::System::GetInstance().GetSystemTimers().GetFakeTimeBase();
        mem_w32(OS_LAST_TIME,     (u32)(tb >> 32));
        mem_w32(OS_LAST_TIME + 4, (u32)tb);
    }

    // Run the guest handler natively: r3 = interrupt, r4 = current OSContext (what the GC handler
    // chain would have passed). The register file seeds from the live ppc so r2/r13 (SDA bases)
    // are valid for OS code; call_ppc returns when the handler blr's.
    t_in_native_dispatch = true;
    if (interrupt == 17 || interrupt == 18) {  // PI_CP / PE_TOKEN: always log (FIFO pacing chain)
        static long cp_logs = 0;
        if (cp_logs++ < 200)
            fprintf(stderr, "[nintr] intr=%d -> handler %08x (cause=%08x tok=%04x r13=%08x cbg=%08x)\n",
                    interrupt, handler, cause, (unsigned)mem_r16(0xCC00100Eu),
                    ppc.gpr[13], mem_r32(0x8040EA18u));
    }
    g_nintr_counts[interrupt & 31]++;
    if (interrupt == 18) g_ds_token_dispatches++;
    // Hardware semantics: taking an external interrupt CLEARS MSR[EE] for the handler (srr1 holds
    // the old MSR; rfi restores it). Without this the handler inherits the interrupted body's
    // EE=1 and Dolphin's interpreter can vector a NESTED guest dispatch mid-handler — observed
    // double-running __DSPHandler, which double-consumed the DSP mail and left the outer handler
    // polling the CPU->DSP mailbox bit forever (the 500M-step run_jit_sync abort, 2026-06-10).
    auto& ppc_msr = Core::System::GetInstance().GetPPCState();
    const u32 saved_ee_msr = ppc_msr.msr.Hex;
    ppc_msr.msr.Hex &= ~0x8000u;
    static const bool dbg_nintr = getenv("SUNBRIGHT_DBG_IDLE") != nullptr;
    if (dbg_nintr) {
        static long d = 0;
        if (d++ < 256)
            fprintf(stderr, "[nintr] dispatch intr=%d handler=%08x (cause=%08x unmasked=%08x) r1=%08x\n",
                    interrupt, handler, cause, unmasked, ppc.gpr[1]);
    }
    CPUState cpu;
    if (seed) cpu = *seed;                             // recomp-boundary delivery: the LIVE thread ctx
    else      dolphin_state_to_cpu(ppc, cpu);          // idle/poll/interp delivery: the global ppc
    cpu.gpr[3] = (u32)interrupt;
    cpu.gpr[4] = mem_r32(0x800000D4u);
    cpu.lr     = sunbright_idle_spin_pc();             // interp ret sentinel if the handler isn't recomp
    // DSP-source handlers may ack exactly their own CSR status bit (see t_dsp_ack_allowed).
    const u32 saved_ack = t_dsp_ack_allowed;
    t_dsp_ack_allowed = (interrupt == 5) ? 0x08u : (interrupt == 6) ? 0x20u
                        : (interrupt == 7) ? 0x80u : 0u;
    call_ppc(cpu, handler);
    t_dsp_ack_allowed = saved_ack;
    ppc_msr.msr.Hex = saved_ee_msr;   // rfi-equivalent: restore the interrupted context's MSR
    t_in_native_dispatch = false;
    return true;
}

// Deliver every currently-dispatchable interrupt natively. Bounded: each handler acks its device
// (dropping the PI cause bit); 16 rounds covers every simultaneous source with room to spare.
static int native_dispatch_pending(const CPUState* seed) {
    int n = 0;
    while (n < 16 && native_dispatch_one(seed)) n++;
    return n;
}

// Recomp-boundary delivery (called from charge_guest_time when the slice expired and the guest's
// logical EE is on). Seeds handlers from the live recomp CPUState so they run on the interrupted
// thread's real stack/SDA. Global ppc is scratch at a recomp boundary, so handler runs through
// call_ppc's interp path are safe.
bool sunbright_deliver_pending_recomp(u32 logical_msr) {
    (void)logical_msr;
    auto& sys = Core::System::GetInstance();
    if (!(sys.GetPPCState().Exceptions & 0x00000004u /*EXCEPTION_EXTERNAL_INT*/)) return false;
    if (t_in_native_dispatch || !g_cur_recomp_cpu) return false;
    return native_dispatch_pending(g_cur_recomp_cpu) > 0;
}

void sunbright_poll_yield() {
    SB_SPIN_GUARD("poll_yield");
    if (g_probe_enabled) g_probe.poll_yield.fetch_add(1, std::memory_order_relaxed);
    auto& sys = Core::System::GetInstance();
    auto& ppc = sys.GetPPCState();
    auto& ct  = sys.GetCoreTiming();
    // Park at a clean idle PC with EE=1 so a device IRQ that becomes pending has a valid srr0 and
    // can vector. Save/restore the guest PC/MSR — they belong to the recomp poll loop we interrupted.
    const u32 idle_pc   = sunbright_idle_spin_pc();
    const u32 saved_pc  = ppc.pc, saved_npc = ppc.npc;
    const u32 saved_msr = ppc.msr.Hex;
    // Sync the LIVE recomp register file into Dolphin before delivering the interrupt. In the C-call
    // model recomp keeps its own CPUState and only commits to Dolphin at a recomp↔JIT boundary, so
    // ppc.gpr[] here is stale (last boundary). An async interrupt preempts the *currently running*
    // context, and the GC exception handler saves the live GPRs (incl. r1, the stack) into the
    // thread's OSContext — so it must see the live recomp registers, not the stale snapshot. Without
    // this the ISR saved/restored a stale stack pointer, eventually surfacing as an r1 whose high bit
    // was lost (a physical/real-mode-looking 0x000xxxxx stack) and a wild-write/bad-entry-sp crash.
    // wait_vi_field already does this (it has the cpu in hand); poll_yield was the missing case.
    if (g_cur_recomp_cpu) cpu_to_dolphin_state(*g_cur_recomp_cpu, ppc);
    ppc.pc = ppc.npc = idle_pc;
    ppc.msr.Hex &= ~0x8000u;                    // EE off: Advance must only make IRQs PENDING —
    ct.Idle();                                  // delivery is native, never the guest vector path.
    ct.Advance();                               // device callback raises the pending IRQ…
    const int delivered = native_dispatch_pending();   // …dispatched natively (handlers via call_ppc)
    if (getenv("SUNBRIGHT_DBG_YIELD") && delivered) {
        static long y = 0;
        if (y++ < 64) fprintf(stderr, "[yield] %d IRQ(s) dispatched natively\n", delivered);
    }
    ppc.msr.Hex = saved_msr;
    ppc.pc = saved_pc; ppc.npc = saved_npc;
}

// PC-port frame-sync replication. The game's render loop blocks on VIWaitForRetrace (vsync) and
// GXDrawDone (GPU finished) via OSSleepThread — GC scheduler parks woken by a HW ISR. On a PC port
// the VI/GP hardware is Dolphin's, so the wait is satisfied by advancing CoreTiming one VI field:
// Dolphin's VI OutputField presents the frame, and the GP FIFO drains. No guest sleep, no scheduler.
// Interrupts are deferred (MSR[EE] cleared) so we never redirect into a guest ISR mid-call; the
// pending VI/GP IRQs are delivered cleanly at the next recomp→JIT boundary.
// 'sc' (syscall) replication. The recompiler stubs every sc to os_hle_call (c_emitter.cpp), and the
// real OS effect lives in the syscall exception handler (vector 0x80000C00) — which only Dolphin has.
// So run the sc under Dolphin's interpreter from its PC: the interpreter takes the syscall exception,
// runs the OS handler, rfi's back, and returns to the caller (cpu.lr). Stubbing it to nothing broke
// every OS operation that goes through sc. (Used by os_hle_call.)
void sunbright_run_syscall(CPUState& cpu, u32 sc_pc) {
    auto& ppc = Core::System::GetInstance().GetPPCState();
    cpu_to_dolphin_state(cpu, ppc);
    ppc.pc = ppc.npc = sc_pc;
    interp_run_until(cpu.lr, 5'000'000);
    dolphin_state_to_cpu(ppc, cpu);
}

// Advance Dolphin's VI one field (presents the frame, drains the GP FIFO) AND deliver every
// interrupt that fires during it. CRUCIAL: the game's per-frame logic and async subsystems run in
// interrupt handlers (the VI retrace callback that advances the scene; audio DMA/DSP). The recomp
// runs on the native C stack and never hits a recomp→JIT boundary inside the frame loop, so if we
// deferred IRQs they'd never be delivered and the game freezes. So we park the guest at the caller's
// continuation (cpu.lr) and let the interpreter take + run each ISR (it rfi's back to cpu.lr), so
// the handlers actually execute before we return to the recomp.
void sunbright_wait_vi_field(CPUState& cpu) {
    // Force ONE VI field of emulated time — the emu-clock half of the frame heartbeat. Without
    // this, emulated time advances only via per-call cycle charging (~0.014x real), Dolphin's VI
    // presents a field every ~1.2 wall-seconds, and everything paced on presented fields (idle
    // retrace, FPS) crawls even though the pipeline is correct (2026-06-10). Delivery is NATIVE:
    // EE stays masked so Advance only makes IRQs pending; native_dispatch_pending runs the
    // handlers. The caller's CPUState is untouched (handlers run on copies / the global ppc).
    auto& sys = Core::System::GetInstance();
    auto& ppc = sys.GetPPCState();
    auto& ct  = sys.GetCoreTiming();
    auto& vi  = sys.GetVideoInterface();
    cpu_to_dolphin_state(cpu, ppc);
    const u32 saved_pc = ppc.pc, saved_npc = ppc.npc, saved_msr = ppc.msr.Hex;
    ppc.pc = ppc.npc = sunbright_idle_spin_pc();
    ppc.msr.Hex &= ~0x8000u;
    const u64 t0 = ct.GetTicks();
    const u64 target = t0 + vi.GetTicksPerField();
    int guard = 0;
    for (; ct.GetTicks() < target && guard < 8192; ++guard) {
        ct.Idle();                                  // skip to the next scheduled device event
        ct.Advance();                               // process it — IRQs become pending only
        native_dispatch_pending();                  // …and are dispatched natively
    }
    {   // field-advance telemetry: is emulated time really moving one field per heartbeat?
        static long calls = 0; static u64 ticks_acc = 0; static int guards_acc = 0;
        ticks_acc += ct.GetTicks() - t0; guards_acc += guard;
        if ((++calls & 63) == 0) {
            extern unsigned long long watchdog_vi_fields();
            fprintf(stderr, "[vi-field] 64 calls: ticks+%llu (target/call=%llu) guards=%d fields=%llu\n",
                    (unsigned long long)ticks_acc, (unsigned long long)vi.GetTicksPerField(),
                    guards_acc, watchdog_vi_fields());
            ticks_acc = 0; guards_acc = 0;
        }
    }
    ppc.msr.Hex = saved_msr;
    ppc.pc = saved_pc; ppc.npc = saved_npc;
}
#endif

void tail_ppc(CPUState& cpu, u32 address) {
#ifdef HAVE_DOLPHIN_CORE
    if (sb_is_wild_branch_target(address)) sb_fatal_wild_branch(address, cpu);
#endif
    if (g_probe_enabled) g_probe.tail.fetch_add(1, std::memory_order_relaxed);
    RecompFunc fn = recomp_lookup(address);
    if (fn) {
        // Tail-to-recomp is a DIRECT dispatch — the call tracers in call_ppc never see it
        // (the fifth execution context; the wave-load chain ran through here invisibly).
        sunbright_trace_jit_entry(address, cpu.gpr[3], cpu.lr);
        fn(cpu); return;
    }
#ifdef HAVE_DOLPHIN_CORE
    // Tail to a bare `blr` (an empty default callback, e.g. the no-op sound-frame hook at
    // 800339a0): executing it just returns to lr — exactly what returning from tail_ppc does in
    // the C-call model. Never worth a siglongjmp handoff that unwinds live native frames
    // (it killed the native audioproc loop at every frame boundary, 2026-06-10).
    if (mem_r32(address) == 0x4E800020u) return;
#endif
    // SUNBRIGHT_DBG_TAIL: log tail-branches to NON-recomp targets — these siglongjmp back to Run,
    // unwinding every recomp C frame in between. If one fires inside a recomp call tree whose caller
    // expected an inline return (a `bl`, not a tail), the caller's epilogue never runs in C and its
    // continuation resumes under Dolphin JIT from the committed state — the boot endRendering clobber.
    static const bool dbg_tail = getenv("SUNBRIGHT_DBG_TAIL") != nullptr;
    if (dbg_tail) {
        static std::unordered_map<u32, unsigned long long> hist;
        static unsigned long long n = 0;
        if (hist.find(address) == hist.end() && hist.size() < 48)
            fprintf(stderr, "[tail-new] non-recomp tail target %08x (lr=%08x)\n", address, cpu.lr);
        hist[address]++;
        if ((++n & 0xFFFF) == 0) {         // every ~64K tails: top non-recomp tail targets
            std::vector<std::pair<u32, unsigned long long>> v(hist.begin(), hist.end());
            std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
            fprintf(stderr, "[tail-hist] top non-recomp tail targets after %llu tails:\n", n);
            for (size_t i = 0; i < v.size() && i < 16; i++)
                fprintf(stderr, "[tail-hist]   %08x  %llu\n", v[i].first, v[i].second);
        }
    }
#ifdef HAVE_DOLPHIN_CORE
    auto& ppc = Core::System::GetInstance().GetPPCState();
    cpu_to_dolphin_state(cpu, ppc);
    ppc.pc = ppc.npc = address;
    if (g_tail_jmp) {
        // Hand the committed state to the CPU loop and unwind every recomp C frame back to Run.
        // NOTE (2026-06-09): this unwinding is NOT safe when an intermediate caller used a non-tail
        // `bl` and still needs a callee-saved register — the unwound frame's C epilogue never runs, so
        // its guest non-volatile is never restored (the boot endRendering→…→vsnprintf r31 clobber).
        // A blanket "run returning tail targets synchronously instead of longjmp" fix was tried and
        // REVERTED: it cleared the logo clobber but hung boot at the OS time-wait func_803433b4 — the
        // longjmp handoff is load-bearing for mftb/CoreTiming-advancing OS waits, which spin forever on
        // the recomp C stack. The correct fix is surgical (avoid the handoff at its source: the libc
        // jump-table interior branches), see docs/native_threading.md.
        siglongjmp(*g_tail_jmp, 1);
    }
#else
    fprintf(stderr, "[sunbright] tail_ppc 0x%08x: no JIT available\n", address);
#endif
}

// SPRs not modeled in CPUState pass straight through to Dolphin's live state so
// the recomp and the JIT agree on HID0/HID2/BATs/etc. Standalone builds use a
// flat array (no HW side effects, but keeps reads/writes self-consistent).
#ifdef HAVE_DOLPHIN_CORE
u32 spr_get(u32 n) {
    return Core::System::GetInstance().GetPPCState().spr[n & 1023];
}
void spr_set(u32 n, u32 v) {
    Core::System::GetInstance().GetPPCState().spr[n & 1023] = v;
}
u32 msr_get() {
    return Core::System::GetInstance().GetPPCState().msr.Hex;
}
void msr_set(u32 v) {
    auto& sys = Core::System::GetInstance();
    sys.GetPPCState().msr.Hex = v;
    sys.GetPowerPC().MSRUpdated();
    sys.GetPowerPC().CheckExceptions();
}
void msr_set_raw(u32 v) {
    // No CheckExceptions(): see intrinsics.h. Keeps Dolphin's derived MSR state
    // coherent (MSRUpdated) but defers interrupt delivery to the JIT boundary.
    auto& sys = Core::System::GetInstance();
    sys.GetPPCState().msr.Hex = v;
    sys.GetPowerPC().MSRUpdated();
}
u64 tb_get() {
    // GetFakeTimeBase() derives the TB live from CoreTiming ticks. ReadFullTimeBaseValue()
    // would return the *stored* spr[TL], which Dolphin only refreshes lazily — it stays
    // frozen while we spin in recomp, so delay loops would never elapse.
    return Core::System::GetInstance().GetSystemTimers().GetFakeTimeBase();
}
#else
static u32 g_spr[1024];
static u32 g_msr;
static u64 g_tb;
u32  spr_get(u32 n)        { return g_spr[n & 1023]; }
void spr_set(u32 n, u32 v) { g_spr[n & 1023] = v; }
u32  msr_get()            { return g_msr; }
void msr_set(u32 v)       { g_msr = v; }
void msr_set_raw(u32 v)   { g_msr = v; }
u64  tb_get()             { return g_tb += 512; }
#endif

#ifdef HAVE_DOLPHIN_CORE
void dolphin_state_to_cpu(const PowerPC::PowerPCState& src, CPUState& dst) {
    for (int i = 0; i < 32; i++) dst.gpr[i] = src.gpr[i];
    for (int i = 0; i < 32; i++) {
        dst.fpr[i].ps0 = src.ps[i].PS0AsDouble();
        dst.fpr[i].ps1 = src.ps[i].PS1AsDouble();
    }
    dst.lr   = src.spr[SPR_LR];
    dst.ctr  = src.spr[SPR_CTR];
    dst.pc   = src.pc;
    // XER — xer_so_ov format: bit1=SO, bit0=OV
    dst.xer.so = src.GetXER_SO();
    dst.xer.ov = src.GetXER_OV();
    dst.xer.ca = src.xer_ca;
    // CR
    u32_to_cr(dst, src.cr.Get());
    // GQR
    for (int i = 0; i < 8; i++) dst.gqr[i] = src.spr[912 + i];
    dst.srr0 = src.spr[26];   // SRR0 — needed now that rfi/exception code is recompiled
    dst.srr1 = src.spr[27];   // SRR1
}

void cpu_to_dolphin_state(const CPUState& src, PowerPC::PowerPCState& dst) {
    for (int i = 0; i < 32; i++) dst.gpr[i] = src.gpr[i];
    for (int i = 0; i < 32; i++) {
        dst.ps[i].SetPS0(src.fpr[i].ps0);
        dst.ps[i].SetPS1(src.fpr[i].ps1);
    }
    dst.spr[SPR_LR]  = src.lr;
    dst.spr[SPR_CTR] = src.ctr;
    dst.pc = src.pc;
    dst.cr.Set(cr_to_u32(src));
    dst.xer_ca = src.xer.ca;
    dst.xer_so_ov = (src.xer.so << 1) | src.xer.ov;   // bit1=SO, bit0=OV
    for (int i = 0; i < 8; i++) dst.spr[912 + i] = src.gqr[i];
    dst.spr[26] = src.srr0;   // SRR0
    dst.spr[27] = src.srr1;   // SRR1
}

// ── Native-threading runtime glue ────────────────────────────────────────────
// Per-guest-thread runtime record, stored in the nthr `user` slot. `ctx` is the global PPC
// register-file snapshot the switch hooks swap with Dolphin's single global PowerPCState on
// each token hand-off (the GC OSContext save/load, done natively). `g_tail_jmp` needs no hook
// — it is thread_local and every guest thread is a real host thread, so it is per-thread free.
namespace {
constexpr u32 OS_CURRENT_THREAD = 0x800000E4u;   // *(this) = OSGetCurrentThread

struct GuestRuntime {
    CPUState ctx;            // global-ppc snapshot for the switch hooks
    u32 saved_msr = 0x00009032u;  // per-thread MSR (NOT in ctx/dolphin_state_to_cpu). Critical: a
                            // thread parked inside OSDisableInterrupts has MSR[EE]=0; without saving
                            // MSR per thread the global ppc.msr leaks across switches → a critical
                            // section runs with interrupts wrongly enabled → heap corruption.
    u32 os_thread = 0;       // guest OSThread*
    u32 entry = 0, param = 0, stack = 0;
    bool is_thread0 = false;
    // Exception-window state, also NOT in CPUState. A thread parked while inside an exception /
    // interrupt-dispatch window (its interp frame suspended mid-ISR) must get ITS OWN SRR0/SRR1
    // and exact npc back on resume — with the global ppc shared, another thread's dispatch
    // overwrites them, and this thread's eventual `rfi` then jumps to a FOREIGN srr0 (observed:
    // "returns" into the middle of an unrelated function whose prologue never ran → epilogue
    // loads a never-written LR slot → blr to 0; also the scrambled __OSDispatchInterrupt →
    // spurious OSError 15 → JUTException crash-screen livelock). The lwarx reservation travels
    // for the same reason. 2026-06-10.
    u32 saved_srr0 = 0, saved_srr1 = 0, saved_npc = 0;
    bool saved_reserve = false;
    u32 saved_reserve_addr = 0;
};

std::unordered_map<u32, nthr::GuestThread*> g_os_to_gt;   // guest OSThread* -> nthr thread
std::mutex                                  g_os_map_mtx;

static void sunbright_dump_guest_threads(FILE* f) {
    std::lock_guard<std::mutex> lk(g_os_map_mtx);
    for (auto& [os_thread, gt] : g_os_to_gt) {
        fprintf(f, "  os_thread=%08x prio(gc)=%d state(gc)=%u suspend=%d srr0=%08x lr=%08x sp=%08x\n",
                os_thread, (int)mem_r32(os_thread + 0x2d0), (unsigned)(mem_r32(os_thread + 0x2c8) >> 16),
                (int)mem_r32(os_thread + 0x2cc), mem_r32(os_thread + 0x198), mem_r32(os_thread + 0x84),
                mem_r32(os_thread + 0x4));
    }
}

GuestRuntime* runtime_of(nthr::GuestThread* t) {
    return static_cast<GuestRuntime*>(nthr::user_slot(t));
}
}  // namespace

static void nthr_ctx_save(nthr::GuestThread* t) {
    auto* gr = runtime_of(t);
    if (!gr) return;
    auto& ppc = Core::System::GetInstance().GetPPCState();
    dolphin_state_to_cpu(ppc, gr->ctx);
    gr->saved_msr = ppc.msr.Hex;   // MSR is NOT in ctx — save it per thread (critical-section EE bit)
    gr->saved_srr0 = ppc.spr[SPR_SRR0];   // exception-window state: see GuestRuntime comment
    gr->saved_srr1 = ppc.spr[SPR_SRR1];
    gr->saved_npc  = ppc.npc;
    gr->saved_reserve      = ppc.reserve;
    gr->saved_reserve_addr = ppc.reserve_address;
    // The running thread's OSThread* is authoritative in 0x800000E4 (it set it). Capture it so the
    // restore hook writes the right current-thread pointer back (thread 0's identity isn't known
    // until boot installs it, after adoption). No map/lock here — the hooks run under nthr's lock.
    // Raw read, NOT mem_r32: a runtime-internal read must not feed the guest spin-loop detector —
    // repeated yields (e.g. a native frame-wait loop) would confirm a "poll" on this EA and
    // sb_poll_fire would advance CoreTiming mid-context-switch on an Undeclared thread.
    u32 cur = 0;
    if (u8* p = sb_ram_fast(OS_CURRENT_THREAD)) { memcpy(&cur, p, 4); cur = __builtin_bswap32(cur); }
    if (cur) gr->os_thread = cur;
}
static void nthr_ctx_restore(nthr::GuestThread* t) {
    auto* gr = runtime_of(t);
    if (!gr) return;
    auto& sys = Core::System::GetInstance();
    cpu_to_dolphin_state(gr->ctx, sys.GetPPCState());
    sys.GetPPCState().msr.Hex = gr->saved_msr;   // restore this thread's MSR (EE/critical-section)
    sys.GetPowerPC().MSRUpdated();                // keep Dolphin's derived MSR flags coherent
    {   // exception-window state back (SRR0/SRR1/npc/reservation — see GuestRuntime comment)
        auto& p2 = sys.GetPPCState();
        p2.spr[SPR_SRR0] = gr->saved_srr0;
        p2.spr[SPR_SRR1] = gr->saved_srr1;
        if (gr->saved_npc) p2.npc = gr->saved_npc;
        p2.reserve         = gr->saved_reserve;
        p2.reserve_address = gr->saved_reserve_addr;
    }
    // We own the current-thread pointer now (the GC scheduler that maintained it is never run),
    // so make OSGetCurrentThread coherent with whoever is about to run. The current-CONTEXT
    // globals (0x800000D4 virtual / 0x800000C0 physical — what OSSetCurrentContext maintains)
    // must follow too: the exception prologue saves srr0 into *0xC0 and __OSDispatchInterrupt
    // exits via OSLoadContext(*0xD4). If they point at a previously-running thread's OSContext,
    // an ISR taken later (e.g. in the idle driver) "returns" into that thread's STALE context —
    // re-running its blocked OSSleepThread call and corrupting the wait queue (the 0x32323502
    // wild read, 2026-06-09). OSContext is at OSThread+0.
    if (gr->os_thread) {
        mem_w32(OS_CURRENT_THREAD, gr->os_thread);
        mem_w32(0x800000D4u, gr->os_thread);
        mem_w32(0x800000C0u, gr->os_thread & 0x7FFFFFFFu);
    }
    static const bool dbg = getenv("SUNBRIGHT_DBG_SWITCH") != nullptr;
    if (dbg) {
        static long n = 0;
        if ((++n & 0xFFF) == 0 || n < 64)
            fprintf(stderr, "[switch #%ld] -> thread %08x pc=%08x sp=%08x\n",
                    n, gr->os_thread, gr->ctx.pc, gr->ctx.gpr[1]);
    }
}

// Native idle / hardware-IRQ driver. Called by nthr (UNLOCKED, on the parking thread's host thread)
// when every guest thread is Blocked waiting on something external — i.e. a hardware wait that only
// a device IRQ can satisfy. Replaces the GC idle thread: advance Dolphin's CoreTiming so the pending
// DSP/DVD/VI device event fires, deliver the external-interrupt exception, and run its ISR — which
// calls native OSWakeupThread → nthr::make_ready, making a guest thread Ready. Loop until one wakes.
//
// The global `ppc` here holds the just-parked thread's context (g_save_hook copied it to the slot but
// did NOT clear ppc), so it has a valid r1 + current-thread pointer for the GC exception handler to
// run on; any mutation we make to it is discarded when the woken thread's context is restored. This
// is the documented poll_yield pattern (park at a clean idle PC with EE=1; Idle()+Advance() raises
// the IRQ; CheckExceptions() vectors to 0x80000500; interp the ISR until it rfi's back) — looped.
// One step of the native idle/hardware driver: park the global ppc at a clean RECOVERABLE idle
// context, fast-forward CoreTiming to the next scheduled device event, deliver any pending external
// interrupt, and run its ISR (→ native OSWakeupThread → nthr::make_ready). Returns true if an IRQ
// was delivered. Caller must be Declared as the CPU thread (single-active under cooperative nthr).
// Mutates global ppc + guest RAM/device state; the register mutations are discarded by the caller
// (idle driver: when a woken thread's ctx is restored; yield path: it restores ppc itself).
//
// A clean RECOVERABLE kernel MSR to take each interrupt on: EE (interrupts on), RI (recoverable —
// the GC handler rejects RI=0 as "Non-recoverable Exception"), ME, IR/DR (translation on). Reset
// every step so a previous exception clearing MSR (real mode, RI=0) doesn't leave the next delivery
// non-recoverable.
// Run the GC idle thread faithfully: spin an idle `b .` under the INTERPRETER. SingleStep is the
// only thing that advances CoreTiming outside the JIT loop — ct.Idle()+Advance() alone does NOT move
// the global timer here (verified: ticks +0), which is why the bare-CoreTiming idle delivered no
// device IRQs (native_threading.md Attempt 1). Each SingleStep consumes downcount → scheduled
// VI/DSP/DVD events fire → with EE on their interrupt is delivered and vectors pc into the ISR; we
// then run the ISR to completion (interp_run_until back to the spin), whose OSWakeupThread is routed
// to nthr::make_ready by the native_os intercept inside interp_run_until. Stops as soon as a thread
// becomes Ready, or after `max_steps` idle steps. Returns whether a thread woke.
static bool idle_run(long max_steps) {
    static const bool dbg = getenv("SUNBRIGHT_DBG_IDLE") != nullptr;
    auto& sys = Core::System::GetInstance();
    auto& ppc = sys.GetPPCState();
    auto& interp = sys.GetInterpreter();
    const u32 idle_pc = sunbright_idle_spin_pc();
    constexpr u32 IDLE_MSR = 0x00009032u;           // EE|ME|IR|DR|RI
    const u64 t0 = sys.GetCoreTiming().GetTicks();
    ppc.pc = ppc.npc = idle_pc;
    ppc.msr.Hex = IDLE_MSR;
    // Point the OS current-context globals at a coherent OSContext for the duration of the idle
    // spin: an exception saves srr0(=idle_pc) into *0x800000C0 and the dispatcher exits via
    // OSLoadContext(*0x800000D4) — they MUST reference the same context or the rfi resumes some
    // parked thread's stale state (see nthr_ctx_restore). The current thread's OSContext is
    // scratch while parked (nthr's authoritative copy is host-side gr->ctx), so borrow it.
    {
        u32 cur = 0;
        if (u8* p = sb_ram_fast(OS_CURRENT_THREAD)) { memcpy(&cur, p, 4); cur = __builtin_bswap32(cur); }
        if (cur) {
            mem_w32(0x800000D4u, cur); mem_w32(0x800000C0u, cur & 0x7FFFFFFFu);
            // The exception prologue runs ON ppc's registers: it stwu's a frame at r1 and ISR code
            // reads SDA globals off r2/r13. The borrowed register file is NOT guaranteed valid here
            // (a just-exited thread leaves r1=0 → exception writes to 0xfffffff8). Give the idle
            // context a dedicated scratch stack in the unused low-mem gap (below our spin at
            // 0x80002FF8) and the real SDA bases from the current OSThread's saved context.
            ppc.gpr[1]  = 0x80002F00u;
            ppc.gpr[2]  = mem_r32(cur + 0x08u);   // OSContext.gpr[2]  (SDA2 base)
            ppc.gpr[13] = mem_r32(cur + 0x34u);   // OSContext.gpr[13] (SDA base)
        }
    }
    long n = 0;
    auto& ct = sys.GetCoreTiming();
    if (dbg) { static int dumps = 0; if (dumps++ < 3)
        fprintf(stderr, "[idle] ticks=%lld\n%s", (long long)ct.GetTicks(),
                ct.GetScheduledEventsSummary().c_str()); }
    for (; n < max_steps; n++) {
        SB_SPIN_GUARD("idle_driver");
        if (nthr::ready_count() > 0) break;
        ppc.msr.Hex = IDLE_MSR & ~0x8000u;          // EE OFF at the spin: IRQs become pending only —
        // Skip straight to the next scheduled device event instead of burning one SingleStep per
        // guest cycle (that capped the whole emulator at ~0.06×). Idle()+Advance() DOES move the
        // global timer now: charge_guest_time keeps the downcount/slice bookkeeping primed outside
        // the JIT loop (the old "ticks +0" failure was unprimed slices, not a CoreTiming property).
        // delivery is NATIVE (native_dispatch_pending), never the guest 0x500 vector → dispatcher →
        // OSLoadContext chain. Stepping that chain under the interpreter is what produced the
        // nested-dispatch corruption (spurious MEMIntrruptHandler → OSError 15, 2026-06-10).
        ct.Idle();
        ct.Advance();
        // Real CP hardware never sleeps while FIFO data is pending — and never SPINS an empty
        // one. Kick the GPU loop only when there is actually data to consume: an unconditional
        // per-step kick kept the Video thread busy-spinning its mainloop at ~99% CPU for nothing
        // (no-busy-spins rule, 2026-06-10). Required when a breakpoint move must let it drain
        // (FIFO-pacing deadlock) — and then distance is nonzero by definition.
        if (sys.GetCommandProcessor().GetFifo().CPReadWriteDistance.load(std::memory_order_relaxed))
            sys.GetFifo().RunGpu();
        int delivered = native_dispatch_pending();
        {   // drawsync loss recovery: GPU parked on the fifo's next boundary + empty queue means
            // its token was PE-coalesced away — post the synthetic token-0 through the normal
            // queue so the real threadFunc advances (sms_drawsync_native.cpp, 2026-06-10).
            CPUState rcpu;
            dolphin_state_to_cpu(ppc, rcpu);
            if (sunbright_drawsync_recover(rcpu)) delivered++;
        }
        // Native retrace from idle: when every guest thread is blocked, the VI retrace
        // transaction (vsync callbacks — incl. the FIFO-breakpoint move that lets the GPU
        // drain and raise the CP underflow resume) must still run once per presented field;
        // on hardware it is an interrupt, not a courtesy of the render loop. (FIFO-pacing
        // deadlock, 2026-06-10.)
        {
            extern bool sunbright_vi_idle_retrace(CPUState&);
            CPUState icpu;
            dolphin_state_to_cpu(ppc, icpu);
            if (sunbright_vi_idle_retrace(icpu)) delivered++;
        }
        if (delivered && dbg) {
            static long v = 0;
            if (v++ < 64)
                fprintf(stderr, "[idle] %d IRQ(s) dispatched natively, ready=%d\n",
                        delivered, nthr::ready_count());
        }
        if (!delivered && ppc.pc == idle_pc) {      // no event fired an IRQ: nudge one step anyway
            sb_ring_push(ppc.pc);
            interp.SingleStep();
        }
    }
    const bool woke = nthr::ready_count() > 0;
    if (dbg) { static long c = 0; if ((c++ & 0xFF) == 0)
        fprintf(stderr, "[idle] %ld steps, ticks +%lld, woke=%d ready=%d\n",
                n, (long long)(sys.GetCoreTiming().GetTicks() - t0), woke, nthr::ready_count()); }
    return woke;
}

static void nthr_idle_driver() {
    Core::DeclareAsCPUThread();   // we run the interpreter here; the parker Undeclared before block()
    const bool woke = idle_run(20'000'000);
    Core::UndeclareAsCPUThread();
    if (!woke) {                                    // genuine deadlock: nothing ever woke
        fflush(stdout);
        fprintf(stderr,
            "\n[nthr] FATAL: idle driver stepped the interpreter, no thread woke (deadlock).\n"
            "  Every guest thread is Blocked and no DSP/DVD/VI IRQ made one Ready.\n"
            "  CoreTiming GetTicks=%llu (compare against the last ctsched 'b' base in /tracelog)\n",
            (unsigned long long)Core::System::GetInstance().GetCoreTiming().GetTicks());
        nthr::dump_threads(stderr);
        fprintf(stderr, "  native dispatch counts:");
        for (int i = 0; i < 32; i++)
            if (g_nintr_counts[i]) fprintf(stderr, " intr%d=%lu", i, g_nintr_counts[i]);
        fputc('\n', stderr);
        sunbright_dump_guest_threads(stderr);   // each thread's guest identity + saved pc/lr/sp
        sunbright_park("nthr idle deadlock");
    }
}

// Body of a spawned guest host thread: runs the guest thread function on its own native stack
// from the entry PC, holding the CPU token. Blocks (parks the host thread) at native OS block
// points; only returns if the guest function actually returns (thread exit).
static void guest_thread_body(u32 os_thread, u32 entry, u32 param, u32 stack) {
    Core::DeclareAsCPUThread();           // we hold the token; become Dolphin's CPU thread
    mem_w32(OS_CURRENT_THREAD, os_thread);

    // Load the initial register context OSCreateThread built (OSContext @ OSThread+0): it holds
    // gpr1=stack, gpr2/gpr13 = small-data (SDA) bases, gpr3=param, srr0=entry, lr=exit trampoline.
    // Hand-setting only sp/param/pc left r2/r13 zero, so every small-data access in the thread
    // computed a wild address (0 − sda_offset = 0xFFFFxxxx) → the strcpy wild-write crash.
    CPUState cpu; cpu.reset();
    for (int i = 0; i < 32; i++) cpu.gpr[i] = mem_r32(os_thread + (u32)(i * 4));
    cpu.lr  = mem_r32(os_thread + 0x84);  // OSContext.lr = exit trampoline
    cpu.ctr = mem_r32(os_thread + 0x88);
    for (int i = 0; i < 8; i++) cpu.gqr[i] = mem_r32(os_thread + 0x1A4 + (u32)(i * 4));
    cpu.pc  = mem_r32(os_thread + 0x198); // OSContext.srr0 = entry
    (void)entry; (void)param; (void)stack;

    const u32 exit_ret = cpu.lr;          // OSContext.lr = exit trampoline (thread done)
    fprintf(stderr, "[nthr] >>> thread %08x body START pc=%08x sp=%08x r3=%08x r13=%08x exit=%08x\n",
            os_thread, cpu.pc, cpu.gpr[1], cpu.gpr[3], cpu.gpr[13], exit_ret);

    if (RecompFunc fn = recomp_lookup(cpu.pc)) {
        sunbright_run_recomp_tree(cpu, fn);            // recomp entry: native C stack
    } else {
        // Non-recomp (JIT-only) entry: run the whole thread under the interpreter, budget-less —
        // it blocks by native parking (OSSleepThread intercept), not by returning, so a step
        // budget would false-positive. Exits only when the thread function returns to its exit
        // trampoline. (If it busy-waits on hardware without ever blocking, that surfaces as a
        // hang — the preemption/idle-driver case, docs step 6.)
        auto& ppc = Core::System::GetInstance().GetPPCState();
        cpu_to_dolphin_state(cpu, ppc);
        ppc.pc = ppc.npc = cpu.pc;
        interp_run_until(exit_ret, /*budget=*/0);
        dolphin_state_to_cpu(ppc, cpu);
    }

    // The guest thread function returned to its exit trampoline (OSExitThread, 0x80348a68). nthr
    // stops AT it, so run OSExitThread's bookkeeping natively (mark MORIBUND / free, release
    // mutexes, wake the join queue) MINUS its GC SelectThread reschedule — otherwise
    // OSIsThreadTerminated / OSJoinThread never see this thread finish (boot-sequencer wait).
    const u32 true_exit_val = cpu.gpr[3];   // capture BEFORE bookkeeping calls clobber r3
    native_os_thread_exit(cpu, os_thread, true_exit_val);

    // Drop it from the map and let nthr reap.
    { std::lock_guard<std::mutex> lk(g_os_map_mtx); g_os_to_gt.erase(os_thread); }
    fprintf(stderr, "[nthr] guest thread %08x (entry %08x) returned/exited (exit_val=%08x)\n", os_thread, entry, true_exit_val);
    Core::UndeclareAsCPUThread();
}

// OSCreateThread → spawn the matching native host thread, SUSPENDED (created threads start with
// suspend=1; OSResumeThread makes it Ready). Maps guest OSThread* ↔ nthr thread.
void nthrt_spawn_guest(u32 os_thread, u32 entry, u32 param, u32 stack, int prio) {
    nthr::GuestThread* gt = nthr::spawn(prio,
        [os_thread, entry, param, stack] { guest_thread_body(os_thread, entry, param, stack); },
        /*start_ready=*/false);
    auto* gr = new GuestRuntime();
    gr->os_thread = os_thread; gr->entry = entry; gr->param = param; gr->stack = stack;
    nthr::user_slot(gt) = gr;
    std::lock_guard<std::mutex> lk(g_os_map_mtx);
    g_os_to_gt[os_thread] = gt;
}

// OSResumeThread / OSWakeupThread → mark the mapped host thread Ready (cooperative: it runs at
// the current thread's next block point).
void nthrt_make_ready(u32 os_thread) {
    nthr::GuestThread* gt = nullptr;
    { std::lock_guard<std::mutex> lk(g_os_map_mtx);
      auto it = g_os_to_gt.find(os_thread); if (it != g_os_to_gt.end()) gt = it->second; }
    if (gt) nthr::make_ready(gt);
}

// Ensure the running guest thread is mapped under its authoritative OSThread* (`os_thread`), so
// a later OSWakeupThread on a queue holding it resolves to this host thread. Needed because
// thread 0's real OSThread* isn't known at adoption time (0x800000E4 is still 0 then). Called off
// nthr's lock (from a blocking primitive), so taking the map lock here is order-safe.
void nthrt_bind_current(u32 os_thread) {
    nthr::GuestThread* gt = nthr::current();
    if (!gt || !os_thread) return;
    if (auto* gr = runtime_of(gt)) gr->os_thread = os_thread;
    std::lock_guard<std::mutex> lk(g_os_map_mtx);
    g_os_to_gt[os_thread] = gt;
}

// OSSleepThread → yield the current guest thread until woken. Brackets the park with
// Undeclare/Declare so exactly one host thread is Dolphin's CPU thread at a time.
void nthrt_block_current() {
    Core::UndeclareAsCPUThread();
    nthr::block(nthr::State::Blocked);
    Core::DeclareAsCPUThread();           // reacquired the token (ctx restored by the hook)
}

// Frame barrier (native VIWaitForRetrace): block until every other Ready thread has run to its
// own block/yield point. The caller's live recomp context must be synced into ppc first so the
// ctx-save hook stashes real state (same rule as nthrt_yield_current).
void nthrt_block_drain(CPUState* caller) {
    if (caller) cpu_to_dolphin_state(*caller, Core::System::GetInstance().GetPPCState());
    Core::UndeclareAsCPUThread();
    nthr::block_drain();
    Core::DeclareAsCPUThread();
}

// Priority preemption point: the current guest thread yields the token but stays RUNNABLE
// (Ready), so the scheduler hands the token to the highest-priority Ready thread — which is how
// the GC scheduler's __OSReschedule (run inside OSResumeThread/OSWakeupThread) switches to a
// just-made-ready higher-priority thread. When that thread later blocks, the token comes back here
// and this returns. Bracketed with Undeclare/Declare like nthrt_block_current.
void nthrt_yield_current(CPUState* yielder) {
    Core::UndeclareAsCPUThread();
    // GC OSYieldThread: hand the CPU to the next runnable thread. The guest calls it inside
    // hardware/time poll loops (TCardManager's CARDProbeEx/__EXIProbe EXI insertion debounce; audio
    // DSP-init waits). On real hardware, if nothing else is runnable the scheduler switches to the
    // IDLE THREAD, which spins with interrupts ON: time keeps passing (the decrementer, VI/DSP/EXI
    // device events keep firing) and their interrupts are DELIVERED, so a blocked thread's wait
    // completes (a VI field wakes the render thread; the EXI debounce elapses as time advances).
    //
    // Under cooperative nthr, recomp runs on the native stack so nothing advances CoreTiming while a
    // guest spin-yields. If nothing else is Ready, replicate the idle thread faithfully: advance
    // CoreTiming AND deliver device IRQs (EE on — NOT deferred) until a thread becomes Ready (e.g.
    // the VI field wakes the render thread) or we've advanced a bounded number of events (then the
    // yielder re-polls — its own time-based wait has progressed). Nothing is special-cased or
    // skipped; the debounce still runs, just with time advancing as on real hardware.
    //
    // The ISR is interpreted on the global ppc, so it needs a VALID stack/context. call_ppc invokes
    // this native override WITHOUT syncing the recomp `cpu` into ppc, so ppc.r1 would be stale —
    // running the ISR on it faulted (wild write to 0xfffffff8). Sync the yielder's live context in
    // first (valid r1/r2/r13), run the idle steps on it, then restore ppc so block()'s save sees the
    // pre-yield context. (Re-entrancy guard: an ISR that itself yields must not recurse the driver.)
    static thread_local bool in_yield_idle = false;
    if (yielder && !in_yield_idle && nthr::ready_count() == 0) {
        in_yield_idle = true;
        auto& ppc = Core::System::GetInstance().GetPPCState();
        alignas(PowerPC::PowerPCState) unsigned char saved[sizeof(PowerPC::PowerPCState)];
        memcpy(saved, &ppc, sizeof(ppc));
        cpu_to_dolphin_state(*yielder, ppc);     // valid stack/context for the interpreted ISR
        Core::DeclareAsCPUThread();
        idle_run(5'000'000);                     // step the idle spin until a thread wakes (e.g. VI → render)
        Core::UndeclareAsCPUThread();
        memcpy(&ppc, saved, sizeof(ppc));        // restore pre-yield ppc; ISR's RAM/device effects kept
        in_yield_idle = false;
    }
    nthr::block(nthr::State::Ready);
    Core::DeclareAsCPUThread();           // reacquired the token (ctx restored by the hook)
}

// GC __OSActiveThreadQueue (OS low mem): every live OSThread is linked here from creation to exit.
//   head @ 0x800000DC, tail @ 0x800000E0 ; OSThread linkActive.next @ +0x2FC, .prev @ +0x300.
// (Extracted from OSCreateThread 0x80348948's __OSLinkActiveThread insert.) Walking it is how we
// enumerate the threads the GC OS already created during early OSInit — BEFORE recomp/native_os
// interception was live — which is the documented blocker for finishing native threading
// (docs/native_threading.md, Attempt 3): adopting only the EmuThread misses them.
namespace {
constexpr u32 OS_ACTIVE_HEAD = 0x800000DCu;
constexpr u32 T_LINK_NEXT    = 0x2FCu;   // OSThread.linkActive.next
constexpr u32 T_OS_STATE     = 0x2C8u;   // u16 state (1=READY,2=RUNNING,4=WAITING,8=MORIBUND)
constexpr u32 T_OS_SUSPEND   = 0x2CCu;   // s32 suspend count
constexpr u32 T_OS_EPRIO     = 0x2D0u;   // s32 effective priority
constexpr u32 T_CTX_SP       = 0x4u;     // OSContext.gpr[1]
constexpr u32 T_CTX_SRR0     = 0x198u;   // OSContext.srr0 (resume PC)

// Walk the active-thread queue and log every thread (verification step toward takeover-time
// adoption). Read-only — no scheduling change.
void log_active_threads(const char* tag);
}  // namespace
// Exposed for native_os.cpp to trigger lazily once the GC has actually created threads (the
// adopt point runs before OS thread-init, when the queue is still empty).
void sunbright_dbg_log_active_threads(const char* tag) { log_active_threads(tag); }
namespace {
void log_active_threads(const char* tag) {
    u32 cur = mem_r32(OS_CURRENT_THREAD);
    fprintf(stderr, "[adopt] %s: active GC threads (current=%08x):\n", tag, cur);
    int n = 0;
    for (u32 th = mem_r32(OS_ACTIVE_HEAD); th >= 0x80000000u && th < 0x81800000u && n < 64;
         th = mem_r32(th + T_LINK_NEXT), ++n) {
        u16 st   = (u16)(mem_r32(th + T_OS_STATE) >> 16);
        s32 susp = (s32)mem_r32(th + T_OS_SUSPEND);
        s32 ep   = (s32)mem_r32(th + T_OS_EPRIO);
        u32 sp   = mem_r32(th + T_CTX_SP);
        u32 srr0 = mem_r32(th + T_CTX_SRR0);
        fprintf(stderr, "  [%d] OSThread=%08x state=%u suspend=%d eprio=%d sp=%08x srr0=%08x%s\n",
                n, th, st, susp, ep, sp, srr0, th == cur ? "  <-- current" : "");
    }
    fprintf(stderr, "[adopt] %s: %d active thread(s)\n", tag, n);
}
}  // namespace

// Takeover-time adoption of the threads the GC OS ALREADY created during early OSInit (before recomp
// interception was live) — the documented blocker (Attempt 3). Walks the active-thread queue and
// registers an nthr GuestThread for every existing guest thread except the running one (= nthr
// thread 0, bound here to its real OSThread*). Adopted threads are spawned PARKED (start_ready=false)
// resuming from their saved OSContext (srr0); nothing makes them Ready yet, so this is INERT — it
// only builds the g_os_to_gt map the native scheduling primitives need. Idempotent; lazy (fires from
// the first native-OS primitive once the queue is populated — adoption itself runs before thread-init
// when the queue is still empty).
void sunbright_adopt_all_gc_threads() {
    u32 cur = mem_r32(OS_CURRENT_THREAD);
    if (!cur || mem_r32(OS_ACTIVE_HEAD) < 0x80000000u) return;   // queue not populated yet
    // Incremental re-scan (NOT one-shot): GC threads are created over the course of boot (the worker
    // pool / audio thread come after the first call), and __OSLinkActiveThread links each into the
    // active queue. So walk the queue every call and adopt any thread not yet in the registry — the
    // map fills in as the OS creates threads, without needing the OSCreateThread intercept.
    nthrt_bind_current(cur);                // nthr thread 0 IS the running GC thread now
    for (u32 th = mem_r32(OS_ACTIVE_HEAD);
         th >= 0x80000000u && th < 0x81800000u;
         th = mem_r32(th + T_LINK_NEXT)) {
        if (th == cur) continue;
        { std::lock_guard<std::mutex> lk(g_os_map_mtx); if (g_os_to_gt.count(th)) continue; }
        const u32 srr0 = mem_r32(th + T_CTX_SRR0);
        const u32 sp   = mem_r32(th + T_CTX_SP);
        const int prio = (int)(s32)mem_r32(th + T_OS_EPRIO);
        nthrt_spawn_guest(th, srr0, 0, sp, prio);   // parked; body resumes from OSContext
        static const bool dbg = getenv("SUNBRIGHT_DBG_ADOPT") != nullptr;
        if (dbg) fprintf(stderr, "[nthr] adopted pre-existing GC thread %08x (srr0=%08x sp=%08x prio=%d)\n",
                         th, srr0, sp, prio);
    }
}

void sunbright_adopt_cpu_thread() {
    static std::once_flag once;
    std::call_once(once, [] {
        nthr::set_switch_hooks(nthr_ctx_save, nthr_ctx_restore);
        nthr::set_idle_handler(nthr_idle_driver);
        nthr::GuestThread* t0 = nthr::adopt_current(/*priority=*/16);
        auto* gr = new GuestRuntime();
        gr->is_thread0 = true;
        gr->os_thread  = mem_r32(OS_CURRENT_THREAD);   // the GC DefaultThread
        nthr::user_slot(t0) = gr;
        if (gr->os_thread) {
            std::lock_guard<std::mutex> lk(g_os_map_mtx);
            g_os_to_gt[gr->os_thread] = t0;
        }
        fprintf(stderr, "[nthr] adopted EmuThread as guest thread 0 (OSThread=%08x, token held)\n",
                gr->os_thread);
        if (getenv("SUNBRIGHT_DBG_ADOPT")) log_active_threads("at-takeover");
    });
}

// A guest stack pointer (r1) is ALWAYS a valid main-RAM address (cached 0x80000000–0x817FFFFF):
// every PPC prologue does `stwu r1, -frame(r1)`. So at a recomp entry, an r1 outside RAM means the
// caller already corrupted the frame/base pointer — the same class as the wild-write trap, but
// caught HERE, at the entry of the bad frame, before its prologue stores spray garbage and bury the
// originator. (This was added after a TBeamManager-ctor crash: a near-NULL `this` only tripped the
// wild-write trap several instructions deep; an entry guard fails faster and names the entry fn.)
static inline bool sb_guest_sp_ok(u32 sp) {
    return (sp >> 28) == 0x8u && (sp & 0x0FFFFFFFu) < 0x01800000u;
}
[[noreturn]] static void sb_fatal_bad_entry_sp(const CPUState& cpu) {
    fflush(stdout);
    fprintf(stderr,
        "\n[sunbright] FATAL bad guest stack pointer at recomp entry: r1=%08x (lr=%08x)\n"
        "  r1 is outside main RAM (0x80000000-0x817FFFFF) — the caller corrupted the stack/base\n"
        "  pointer before this call. Failing at the entry frame (earlier than the wild-write trap).\n"
        "  Native backtrace (recomp call chain, innermost first):\n",
        cpu.gpr[1], cpu.lr);
    void* bt[96];
    int bn = backtrace(bt, 96);
    backtrace_symbols_fd(bt, bn, fileno(stderr));
    fflush(stderr);
    struct rlimit no_core{0, 0};   // skip the multi-GB core dump (see the step-budget trap above)
    setrlimit(RLIMIT_CORE, &no_core);
    abort();
}

void sunbright_run_recomp_tree(CPUState& cpu, void (*fn)(CPUState&)) {
    if (!sb_guest_sp_ok(cpu.gpr[1])) sb_fatal_bad_entry_sp(cpu);
    sigjmp_buf jb;
    sigjmp_buf* prev = sunbright_set_tail_jmp(&jb);
    CPUState* prev_cpu = g_cur_recomp_cpu; g_cur_recomp_cpu = &cpu;   // for fault diagnostics
    struct CpuRestore { CPUState* p; ~CpuRestore() { g_cur_recomp_cpu = p; } } cpu_restore{prev_cpu};
    if (sigsetjmp(jb, 0) == 0) {
        fn(cpu);
        // Normal C return (top-level blr): commit our state and continue at the return addr.
        auto& ppc = Core::System::GetInstance().GetPPCState();
        cpu_to_dolphin_state(cpu, ppc);
        ppc.pc = ppc.npc = cpu.lr;
    }
    // else: a tail-branch into non-recomp code siglongjmp'd back, having already committed ppc.
    sunbright_set_tail_jmp(prev);
}
#endif

// JIT-entry twin of the call_ppc tracers (see sunbright_bridge.cpp Run): logs the same DBG_NOTE
// address set when a recompiled function is entered FROM A JIT CONTEXT (which bypasses call_ppc).
// Tagged "jnote" so trace analysis can tell the contexts apart.
void sunbright_trace_jit_entry(u32 address, u32 r3, u32 lr) {
    static const bool dbg_note = getenv("SUNBRIGHT_DBG_NOTE") != nullptr;
    if (!dbg_note) return;
    switch (address) {
    case 0x8030dc7cu: case 0x8031ab50u: case 0x80312790u: case 0x8031273cu:
    case 0x80314660u: case 0x80311550u: case 0x80311708u: case 0x8031c914u:
    case 0x8031ce68u: case 0x8031defcu: case 0x8031deb4u: case 0x8031dd00u:
    case 0x8031c818u: case 0x803068d8u: case 0x803017b0u:
    case 0x80301850u: case 0x80301884u: case 0x80310994u: case 0x80310694u:
    case 0x80015640u: case 0x8001569cu: case 0x802bc10cu: case 0x802bb920u: case 0x80318050u:
    case 0x8031c894u: {
        // stopSeq via jit/tail context: also direct stderr under DBG_WAVE (ring drops events).
        static const bool dw = getenv("SUNBRIGHT_DBG_WAVE") != nullptr;
        if (dw) fprintf(stderr, "[wave-jt] call %08x r3=%08x lr=%08x\n", address, r3, lr);
        struct timespec ts2; clock_gettime(CLOCK_MONOTONIC, &ts2);
        sb_trace("jnote", address, r3, lr, (u32)(ts2.tv_sec * 1000 + ts2.tv_nsec / 1000000));
        break;
    }
    case 0x802b76f4u: case 0x802b77ecu: case 0x802b77fcu: case 0x802b7898u:
    case 0x80299838u: case 0x80348d08u: case 0x80348374u: {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        sb_trace("jnote", address, r3, lr, (u32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000));
        break;
    }
    default: break;
    }
}

// Interpreter-context tracer (see interp loop): same DBG_NOTE set, tag "inote".
void sunbright_trace_interp_pc(u32 pc, u32 r3, u32 lr) {
    static const bool dbg_note = getenv("SUNBRIGHT_DBG_NOTE") != nullptr;
    if (!dbg_note) return;
    switch (pc) {
    case 0x8030dc7cu: case 0x8031ab50u: case 0x8031c914u: case 0x8031ce68u:
    case 0x803017b0u: case 0x80301850u: case 0x80301884u: case 0x80310994u:
    case 0x80310694u: case 0x80015640u: case 0x8001569cu: case 0x802bb920u:
    case 0x802bc10cu: case 0x802b76f4u: case 0x802b77ecu: case 0x802b77fcu:
    case 0x802b7898u: case 0x80299838u: {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        sb_trace("inote", pc, r3, lr, (u32)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000));
        break;
    }
    default: break;
    }
}
