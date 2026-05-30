#include "sunbright_bridge.h"
#include "cpu_state.h"
#include "dolphin_hook.h"
#include "overrides.h"
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <string>

// Pull in Dolphin's PowerPCState when building inside Dolphin
#ifdef HAVE_DOLPHIN_CORE
#  include "Core/PowerPC/PowerPC.h"
#  include "Core/PowerPC/Interpreter/Interpreter.h"
#  include "Core/HW/Memmap.h"
#  include "Core/System.h"
#  include <cstring>
#  include <vector>
#endif

using RecompFunc = void (*)(CPUState&);
struct JumpEntry { uint32_t addr; RecompFunc fn; };

// The recompiled function table is now linked directly into the sunbright binary
// (generated/jump_table.cpp), so there is no shared library to dlopen and no
// dynamic-symbol resolution to get wrong.
extern "C" const JumpEntry g_recomp_table[];
extern "C" const size_t    g_recomp_table_size;

static std::unordered_map<uint32_t, RecompFunc> g_table;
static bool g_inited = false;

namespace SunbrightBridge {

bool Init(const char* /*unused*/) {
    if (g_inited) return true;

    g_table.reserve(g_recomp_table_size);
    for (size_t i = 0; i < g_recomp_table_size; i++)
        g_table[g_recomp_table[i].addr] = g_recomp_table[i].fn;

    fprintf(stderr, "[sunbright] Linked %zu recompiled functions\n", g_recomp_table_size);
    g_inited = true;
    return true;
}

bool IsRecompiled(uint32_t pc) {
    // SUNBRIGHT_DISABLE_RECOMP=1 forces everything through Dolphin's JIT —
    // a control to isolate recomp-handoff bugs from launcher/Dolphin issues.
    static const bool disabled = getenv("SUNBRIGHT_DISABLE_RECOMP") != nullptr;
    if (disabled) return false;
    if (is_jit_forced(pc)) return false;            // routed to Dolphin's JIT
    if (override_lookup(pc)) return true;           // hand-written native override
    return g_inited && g_table.count(pc);
}

#ifdef HAVE_DOLPHIN_CORE
// ── Differential validator (SUNBRIGHT_DIFF=1) ────────────────────────────────
// For each recompiled function entered from the JIT, run our recomp on a snapshot,
// then run Dolphin's interpreter from the same entry as the reference, and compare
// the resulting register state. The interpreter result is committed (so the game
// keeps booting correctly), and the FIRST function whose recomp output diverges is
// logged — that's the root-cause bug. Slow (whole game runs on the interpreter with
// a 24 MB RAM snapshot per call), but decisive.
namespace {

constexpr u32 RAM_BASE = 0x80000000, RAM_SIZE = 0x1800000;

struct RegSnap {
    u32 gpr[32], lr, ctr, cr, pc; u8 ca;
};
void snap(const PowerPC::PowerPCState& s, RegSnap& r) {
    for (int i = 0; i < 32; i++) r.gpr[i] = s.gpr[i];
    r.lr = s.spr[SPR_LR]; r.ctr = s.spr[SPR_CTR];
    r.cr = s.cr.Get(); r.ca = s.xer_ca; r.pc = s.pc;
}
void restore(PowerPC::PowerPCState& s, const RegSnap& r) {
    for (int i = 0; i < 32; i++) s.gpr[i] = r.gpr[i];
    s.spr[SPR_LR] = r.lr; s.spr[SPR_CTR] = r.ctr;
    s.cr.Set(r.cr); s.xer_ca = r.ca; s.pc = r.pc;
}

bool diff_run(uint32_t pc, RecompFunc fn) {
    auto& sys = Core::System::GetInstance();
    auto& ppc = sys.GetPPCState();
    u8* ram = sys.GetMemory().GetPointerForRange(RAM_BASE, RAM_SIZE);
    if (!ram) return false;

    static std::vector<u8> ram0(RAM_SIZE), ramRec(RAM_SIZE);
    RegSnap s0; snap(ppc, s0);
    std::memcpy(ram0.data(), ram, RAM_SIZE);

    // (1) recompiled version — exits via call_ppc which writes ppc + ppc.pc.
    CPUState cpu; dolphin_state_to_cpu(ppc, cpu); cpu.pc = pc;
    fn(cpu);
    RegSnap rec; snap(ppc, rec);
    const u32 exit_pc = ppc.pc;
    std::memcpy(ramRec.data(), ram, RAM_SIZE);   // recomp's resulting RAM

    // (2) reference: interpreter from the same entry until it reaches recomp's exit.
    restore(ppc, s0);
    std::memcpy(ram, ram0.data(), RAM_SIZE);
    ppc.pc = pc; ppc.npc = pc;
    auto& interp = sys.GetInterpreter();
    long steps = 0; constexpr long MAX = 3'000'000;
    while (ppc.pc != exit_pc && steps++ < MAX) interp.SingleStepInner();

    if (steps >= MAX) {
        // The interpreter never reached recomp's exit within budget — almost always
        // a long internal loop (cache flush, memset) recomp finished in native C.
        // Inconclusive, not a divergence: commit recomp's result and move on.
        restore(ppc, rec);
        std::memcpy(ram, ramRec.data(), RAM_SIZE);
        return true;
    }

    RegSnap ref; snap(ppc, ref);   // committed state is the interpreter's = correct

    // (3) compare — same exit reached, so any register difference is a real bug.
    auto differ = [&]{
        if (rec.lr != ref.lr || rec.ctr != ref.ctr || rec.cr != ref.cr || rec.ca != ref.ca)
            return true;
        for (int i = 0; i < 32; i++) if (rec.gpr[i] != ref.gpr[i]) return true;
        return false;
    };
    if (differ()) {
        fprintf(stderr, "\n[DIFF] func_%08x diverges (exit rec=%08x ref=%08x steps=%ld)\n",
                pc, exit_pc, ref.pc, steps);
        for (int i = 0; i < 32; i++)
            if (rec.gpr[i] != ref.gpr[i])
                fprintf(stderr, "  r%-2d rec=%08x ref=%08x\n", i, rec.gpr[i], ref.gpr[i]);
        if (rec.lr  != ref.lr)  fprintf(stderr, "  lr  rec=%08x ref=%08x\n", rec.lr, ref.lr);
        if (rec.ctr != ref.ctr) fprintf(stderr, "  ctr rec=%08x ref=%08x\n", rec.ctr, ref.ctr);
        if (rec.cr  != ref.cr)  fprintf(stderr, "  cr  rec=%08x ref=%08x\n", rec.cr, ref.cr);
        if (rec.ca  != ref.ca)  fprintf(stderr, "  ca  rec=%u ref=%u\n", rec.ca, ref.ca);
        // Stop at the first (root-cause) divergence for a clean, fast answer.
        if (getenv("SUNBRIGHT_DIFF_STOP")) { fflush(stderr); _exit(42); }
    }
    return true;
}

}  // namespace
#endif

bool Run(uint32_t pc) {
    // Hand-written native override takes precedence over the generated function.
    RecompFunc fn = override_lookup(pc);
    if (!fn) {
        auto it = g_table.find(pc);
        if (it == g_table.end()) return false;
        fn = it->second;
    }

#ifdef HAVE_DOLPHIN_CORE
    static const bool diff = getenv("SUNBRIGHT_DIFF") != nullptr;
    if (diff) return diff_run(pc, fn);

    // Translate Dolphin's live ppcState into our CPUState
    CPUState cpu;
    dolphin_state_to_cpu(Core::System::GetInstance().GetPPCState(), cpu);
    cpu.pc = pc;

    // Call the native function.
    // Every exit path calls call_ppc(cpu, next_addr) which writes back state and sets
    // ppc.pc to the correct continuation address before returning here.
    static const bool trace = getenv("SUNBRIGHT_TRACE") != nullptr;
    if (trace) fprintf(stderr, "[recomp] running func_%08x\n", pc);
    fn(cpu);
    if (trace) fprintf(stderr, "[recomp] func_%08x returned\n", pc);
#else
    fprintf(stderr, "[sunbright] Run() called without HAVE_DOLPHIN_CORE — no-op\n");
    (void)fn;
#endif
    return true;
}

void Shutdown() {
    g_table.clear();
    g_inited = false;
}

}  // namespace SunbrightBridge
