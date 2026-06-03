#include "dolphin_hook.h"
#include "overrides.h"
#include "native_threads.h"
#include "native_os.h"
#include "probe_server.h"
#include <dlfcn.h>
#include <mutex>
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
#include <execinfo.h>
#include <sys/resource.h>
#ifdef HAVE_DOLPHIN_CORE
#  include "Core/System.h"
#  include "Core/Core.h"
#  include "Core/CoreTiming.h"
#  include "Core/HW/VideoInterface.h"
#endif

extern void mem_w32(u32 ea, u32 v);   // from memory_bridge
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
bool interp_run_until(u32 ret, long budget);   // defined below; used by call_ppc

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
#endif

void call_ppc(CPUState& cpu, u32 address) {
    os_sync_watch(address, cpu.gpr[3], cpu.gpr[4], cpu.lr);
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
    cpu_to_dolphin_state(cpu, ppc);
    ppc.pc = ppc.npc = address;
    // A context switch never returns to us — hand off to Dolphin's CPU loop instead of spinning.
    if (address == OS_LOAD_CONTEXT && g_tail_jmp) siglongjmp(*g_tail_jmp, 1);
    constexpr long MAX = 500'000'000;
    if (!interp_run_until(ret, MAX)) {
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
bool interp_run_until(u32 ret, long budget) {
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
    while (ppc.pc != ret) {
        if (budget) { if (n++ >= budget) return false; }
        else if ((++n & 0x7FFFFFF) == 0)   // budget-less (thread body): periodic progress probe
            fprintf(stderr, "[interp] thread-body still running pc=%08x after %ldM steps\n",
                    ppc.pc, n / 1'000'000);
        if (NativeOSFn nf = native_os_lookup(ppc.pc)) {
            static bool first = true;
            if (first) { first = false;
                fprintf(stderr, "[native_os] first interpreter-path intercept at %08x\n", ppc.pc); }
            CPUState t; dolphin_state_to_cpu(ppc, t); t.pc = ppc.pc;
            nf(t);
            cpu_to_dolphin_state(t, ppc);
            ppc.pc = ppc.npc = t.lr;   // return to the guest caller (LR), like the callee's blr
            continue;
        }
        os_sync_watch(ppc.pc, ppc.gpr[3], ppc.gpr[4], ppc.spr[SPR_LR]);
        interp.SingleStep();
        if (g_probe_enabled) g_probe.interp_steps.fetch_add(1, std::memory_order_relaxed);
    }
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
    static u32 cached = 0;
    if (cached) return cached;
    for (u32 a = 0x80003100u; a < 0x80040000u; a += 4)
        if (mem_r32(a) == 0x48000000u) { cached = a; break; }
    if (!cached) cached = 0x80003100u;
    return cached;
}
void sunbright_poll_yield() {
    if (g_probe_enabled) g_probe.poll_yield.fetch_add(1, std::memory_order_relaxed);
    auto& sys = Core::System::GetInstance();
    auto& ppc = sys.GetPPCState();
    auto& ct  = sys.GetCoreTiming();
    // Park at a clean idle PC with EE=1 so a device IRQ that becomes pending has a valid srr0 and
    // can vector. Save/restore the guest PC/MSR — they belong to the recomp poll loop we interrupted.
    const u32 idle_pc   = sunbright_idle_spin_pc();
    const u32 saved_pc  = ppc.pc, saved_npc = ppc.npc;
    const u32 saved_msr = ppc.msr.Hex;
    ppc.pc = ppc.npc = idle_pc;
    ppc.msr.Hex |= 0x8000u;
    ct.Idle();                                  // fast-forward to the next scheduled device event
    ct.Advance();                               // process it — a device callback raises a pending IRQ
    sys.GetPowerPC().CheckExceptions();         // …which Advance does NOT deliver: vector pc to the ISR
    if (ppc.pc != idle_pc)                      // an interrupt was delivered → run its handler
        interp_run_until(idle_pc, 5'000'000);   // ISR sets the polled flag / calls OSWakeupThread, rfi's back
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
    auto& sys = Core::System::GetInstance();
    auto& ppc = sys.GetPPCState();
    auto& ct  = sys.GetCoreTiming();
    auto& vi  = sys.GetVideoInterface();
    cpu_to_dolphin_state(cpu, ppc);
    ppc.pc = ppc.npc = cpu.lr;                      // continuation an ISR will rfi back to
    const u64 target = ct.GetTicks() + vi.GetTicksPerField();
    for (int guard = 0; ct.GetTicks() < target && guard < 8192; ++guard) {
        const u32 ret = ppc.pc;
        ct.Idle();                                  // skip to the next scheduled device event
        ct.Advance();                               // process it; if EE & pending, vector to the ISR
        if (ppc.pc != ret)                          // an interrupt fired → run its handler + callbacks
            interp_run_until(ret, 5'000'000);
    }
    dolphin_state_to_cpu(ppc, cpu);
}
#endif

void tail_ppc(CPUState& cpu, u32 address) {
    if (g_probe_enabled) g_probe.tail.fetch_add(1, std::memory_order_relaxed);
    RecompFunc fn = recomp_lookup(address);
    if (fn) { fn(cpu); return; }   // tail to recomp → nested call; the caller then returns
#ifdef HAVE_DOLPHIN_CORE
    auto& ppc = Core::System::GetInstance().GetPPCState();
    cpu_to_dolphin_state(cpu, ppc);
    ppc.pc = ppc.npc = address;
    if (g_tail_jmp) {
        // Hand the committed state to the CPU loop and unwind every recomp C frame
        // back to Run. Correct even when an intermediate caller was a non-tail `bl`:
        // its continuation simply resumes under the JIT from the shared state instead
        // of on the C stack.
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
    u32 os_thread = 0;       // guest OSThread*
    u32 entry = 0, param = 0, stack = 0;
    bool is_thread0 = false;
};

std::unordered_map<u32, nthr::GuestThread*> g_os_to_gt;   // guest OSThread* -> nthr thread
std::mutex                                  g_os_map_mtx;

GuestRuntime* runtime_of(nthr::GuestThread* t) {
    return static_cast<GuestRuntime*>(nthr::user_slot(t));
}
}  // namespace

static void nthr_ctx_save(nthr::GuestThread* t) {
    auto* gr = runtime_of(t);
    if (!gr) return;
    dolphin_state_to_cpu(Core::System::GetInstance().GetPPCState(), gr->ctx);
    // The running thread's OSThread* is authoritative in 0x800000E4 (it set it). Capture it so the
    // restore hook writes the right current-thread pointer back (thread 0's identity isn't known
    // until boot installs it, after adoption). No map/lock here — the hooks run under nthr's lock.
    if (u32 cur = mem_r32(OS_CURRENT_THREAD)) gr->os_thread = cur;
}
static void nthr_ctx_restore(nthr::GuestThread* t) {
    auto* gr = runtime_of(t);
    if (!gr) return;
    cpu_to_dolphin_state(gr->ctx, Core::System::GetInstance().GetPPCState());
    // We own the current-thread pointer now (the GC scheduler that maintained it is never run),
    // so make OSGetCurrentThread coherent with whoever is about to run.
    if (gr->os_thread) mem_w32(OS_CURRENT_THREAD, gr->os_thread);
}

// All guest threads are Blocked waiting on hardware and there is no idle/driver yet to advance
// device timing and deliver the waking IRQ. Fail fast (suppress the multi-GB core — see
// [[abort-coredump-hang]]) so this surfaces as a diagnosable signal, not a silent hang.
static void nthr_idle_fatal() {
    fflush(stdout);
    fprintf(stderr,
        "\n[nthr] FATAL: all guest threads Blocked, none Ready — reached a hardware-wait idle.\n"
        "  The native idle/driver (advance CoreTiming so a DSP/DVD/VI IRQ handler wakes a\n"
        "  waiter) is not implemented yet — this is the next step (docs step 5/6).\n");
    fflush(stderr);
    struct rlimit no_core{0, 0};
    setrlimit(RLIMIT_CORE, &no_core);
    abort();
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

    // The guest thread function returned (exited). Drop it from the map and let nthr reap.
    { std::lock_guard<std::mutex> lk(g_os_map_mtx); g_os_to_gt.erase(os_thread); }
    fprintf(stderr, "[nthr] guest thread %08x (entry %08x) returned/exited\n", os_thread, entry);
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

void sunbright_adopt_cpu_thread() {
    static std::once_flag once;
    std::call_once(once, [] {
        nthr::set_switch_hooks(nthr_ctx_save, nthr_ctx_restore);
        nthr::set_idle_handler(nthr_idle_fatal);
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
    });
}

void sunbright_run_recomp_tree(CPUState& cpu, void (*fn)(CPUState&)) {
    sigjmp_buf jb;
    sigjmp_buf* prev = sunbright_set_tail_jmp(&jb);
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
