#include "dolphin_hook.h"
#include "overrides.h"
#include <dlfcn.h>
#include <cstdlib>
#ifdef HAVE_DOLPHIN_CORE
#  include "Core/HW/SystemTimers.h"
#  include "Core/PowerPC/Interpreter/Interpreter.h"
#endif
#include <cstdio>
#include <cstring>
#include <csetjmp>
#include <algorithm>
#include <unordered_map>
#ifdef HAVE_DOLPHIN_CORE
#  include "Core/System.h"
#endif

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
static u32 watch_addr() {
    static const u32 a = getenv("SUNBRIGHT_WATCH")
                         ? (u32)strtoul(getenv("SUNBRIGHT_WATCH"), nullptr, 16) : 0;
    return a;
}

void call_ppc(CPUState& cpu, u32 address) {
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
    RecompFunc fn = recomp_lookup(address);
    if (fn) {
        // Recompiled target → nested native call. Control comes back here when the
        // callee returns (its blr is a C return); the caller then continues inline.
        fn(cpu);
        return;
    }
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
    auto& interp = Core::System::GetInstance().GetInterpreter();
    // SingleStep (not SingleStepInner) advances CoreTiming and checks exceptions each
    // instruction, so HW-wait/spin loops inside the callee (EXI/DI/VI/DSP polling)
    // make progress and their interrupts actually fire.
    constexpr long MAX = 500'000'000;
    long n = 0;
    while (ppc.pc != ret && n++ < MAX) interp.SingleStep();
    if (n >= MAX)
        fprintf(stderr, "[sunbright] run_jit_sync(%08x→%08x) exceeded step budget\n", address, ret);
    dolphin_state_to_cpu(ppc, cpu);
#else
    fprintf(stderr, "[sunbright] call_ppc 0x%08x: not recompiled and no JIT available\n", address);
#endif
}

// ── Tail-branch handoff ──────────────────────────────────────────────────────
// Set by SunbrightBridge::Run for the duration of a top-level recomp entry so a
// tail-branch into non-recomp code can siglongjmp back out, abandoning the recomp
// C-call stack (which is exactly what the PPC tail branch does to its own stack).
static thread_local sigjmp_buf* g_tail_jmp = nullptr;
sigjmp_buf* sunbright_set_tail_jmp(sigjmp_buf* j) {
    sigjmp_buf* prev = g_tail_jmp; g_tail_jmp = j; return prev;
}

void tail_ppc(CPUState& cpu, u32 address) {
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
}
#endif
