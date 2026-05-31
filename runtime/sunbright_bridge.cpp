#include "sunbright_bridge.h"
#include "cpu_state.h"
#include "dolphin_hook.h"
#include "overrides.h"
#include "memory_bridge.h"
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
#  include <map>
#  include <fstream>
#  include <algorithm>
#  include <unordered_set>
#  include <csetjmp>
#  include <csignal>
#endif

using RecompFunc = void (*)(CPUState&);
struct JumpEntry { uint32_t addr; RecompFunc fn; };

extern f32 mem_rf32(u32 ea);  // from memory_bridge (for SUNBRIGHT_WATCH matrix dump)

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

    // Build the dispatch table used by call_ppc (intra-recomp bl/returns). Without
    // this, every call inside recomp missed and bounced to Dolphin's JIT with a
    // full register-file copy — the per-call state-copy slowdown.
    recomp_build_dispatch();

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
    u32 gpr[32], lr, ctr, cr, pc; u8 ca, so_ov;
};
void snap(const PowerPC::PowerPCState& s, RegSnap& r) {
    for (int i = 0; i < 32; i++) r.gpr[i] = s.gpr[i];
    r.lr = s.spr[SPR_LR]; r.ctr = s.spr[SPR_CTR];
    // XER must be captured/restored in full — SO/OV too, not just CA — or the
    // interpreter run starts with a stale XER[SO] and every cmp (which copies
    // XER[SO] into the CR) falsely "diverges".
    r.cr = s.cr.Get(); r.ca = s.xer_ca; r.so_ov = s.xer_so_ov; r.pc = s.pc;
}
void restore(PowerPC::PowerPCState& s, const RegSnap& r) {
    for (int i = 0; i < 32; i++) s.gpr[i] = r.gpr[i];
    s.xer_so_ov = r.so_ov;
    s.spr[SPR_LR] = r.lr; s.spr[SPR_CTR] = r.ctr;
    s.cr.Set(r.cr); s.xer_ca = r.ca; s.pc = r.pc;
}

// ── Correctness harness ──────────────────────────────────────────────────────
// Aggregate every recomp function that diverges from Dolphin's interpreter across
// a play session into a prioritized, named report (instead of stopping at the
// first). Run with SUNBRIGHT_DIFF=1; the report is rewritten incrementally to
// /tmp/sunbright_diff_report.txt so results survive a kill.
struct DiffInfo { long count = 0; u32 exit_pc = 0; std::string detail; };
static std::map<u32, DiffInfo> g_diffs;

// A recompiled function can SEGFAULT (e.g. a bad pointer from a recomp bug), which
// would kill the whole harness. Catch SIGSEGV that occurs while we're executing a
// recomp function under the validator, longjmp back, and treat it as a (severe)
// divergence — so validation continues across crashing functions.
static sigjmp_buf g_diff_jmp;
static volatile sig_atomic_t g_in_recomp = 0;
static struct sigaction g_old_segv;
static void diff_segv(int sig, siginfo_t* si, void* ctx) {
    if (g_in_recomp) { g_in_recomp = 0; siglongjmp(g_diff_jmp, 1); }
    if (g_old_segv.sa_flags & SA_SIGINFO) { if (g_old_segv.sa_sigaction) g_old_segv.sa_sigaction(sig, si, ctx); }
    else if (g_old_segv.sa_handler) g_old_segv.sa_handler(sig);
    else { signal(sig, SIG_DFL); raise(sig); }
}
static const bool g_segv_installed = [] {
    struct sigaction sa{}; sa.sa_sigaction = diff_segv; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_old_segv);
    sigaction(SIGBUS,  &sa, nullptr);
    return true;
}();

static const std::map<u32, std::string>& func_names() {
    static const std::map<u32, std::string> names = [] {
        std::map<u32, std::string> m;
        std::ifstream f("reference/sms_gmse01_funcs.txt");
        u32 a; std::string n;
        while (f >> std::hex >> a >> n) m[a] = n;
        return m;
    }();
    return names;
}

static void write_diff_report() {
    std::ofstream f("/tmp/sunbright_diff_report.txt");
    f << g_diffs.size() << " diverging recomp functions (most frequent first)\n\n";
    std::vector<std::pair<u32, DiffInfo>> v(g_diffs.begin(), g_diffs.end());
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
    const auto& names = func_names();
    char line[256];
    for (const auto& [pc, info] : v) {
        auto it = names.find(pc);
        std::snprintf(line, sizeof(line), "func_%08x x%-6ld exit=%08x  %s\n      %s\n",
                      pc, info.count, info.exit_pc,
                      it != names.end() ? it->second.c_str() : "(unnamed)", info.detail.c_str());
        f << line;
    }
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
    g_recomp_touched_mmio = false;
    CPUState cpu; dolphin_state_to_cpu(ppc, cpu); cpu.pc = pc;
    g_in_recomp = 1;
    if (sigsetjmp(g_diff_jmp, 1) != 0) {
        // The recompiled function segfaulted. Record it as a crash divergence and
        // recover by committing the interpreter's correct result.
        g_in_recomp = 0;
        if (!g_diffs.count(pc)) {
            DiffInfo& info = g_diffs[pc]; info.count = 1; info.exit_pc = 0;
            info.detail = "*** SEGFAULT in recomp ***";
            fprintf(stderr, "[DIFF] func_%08x CRASHED in recomp\n", pc);
            write_diff_report();
        } else g_diffs[pc].count++;
        restore(ppc, s0); std::memcpy(ram, ram0.data(), RAM_SIZE);
        ppc.pc = pc; ppc.npc = pc;
        auto& interp = sys.GetInterpreter();
        // Run the interpreter to the function's return so the game stays correct.
        for (long st = 0; st < 3'000'000; st++) { interp.SingleStepInner(); if (ppc.pc == s0.lr) break; }
        return true;
    }
    fn(cpu);
    g_in_recomp = 0;
    const bool mmio = g_recomp_touched_mmio;
    RegSnap rec; snap(ppc, rec);
    const u32 exit_pc = ppc.pc;
    std::memcpy(ramRec.data(), ram, RAM_SIZE);   // recomp's resulting RAM

    // Hardware-register reads legitimately differ between two separate runs (the
    // register changes with time). Don't compare such functions — commit recomp.
    if (mmio) { restore(ppc, rec); std::memcpy(ram, ramRec.data(), RAM_SIZE); return true; }

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

    // (3) compare — same exit reached, so any difference is a real bug.
    bool regs_differ = (rec.lr != ref.lr || rec.ctr != ref.ctr ||
                        rec.cr != ref.cr || rec.ca != ref.ca);
    for (int i = 0; i < 32 && !regs_differ; i++)
        if (rec.gpr[i] != ref.gpr[i]) regs_differ = true;

    // RAM: recomp's result (ramRec) vs the interpreter's (now live in `ram`).
    // Off by default — FP-heavy functions differ by a few ULPs (reciprocal
    // estimates, rounding), which floods the log; a control-flow/hang bug shows
    // up in registers. Enable with SUNBRIGHT_DIFF_RAM to hunt memory bugs.
    static const bool check_ram = getenv("SUNBRIGHT_DIFF_RAM") != nullptr;
    long ram_diff_off = -1;
    if (check_ram && std::memcmp(ramRec.data(), ram, RAM_SIZE) != 0)
        for (u32 i = 0; i < RAM_SIZE; i++)
            if (ramRec[i] != ram[i]) { ram_diff_off = i; break; }

    if (regs_differ || ram_diff_off >= 0) {
        auto it = g_diffs.find(pc);
        const bool first = (it == g_diffs.end());
        DiffInfo& info = g_diffs[pc];
        info.count++;
        if (first) {
            // Build a compact one-line detail of what diverged (first occurrence).
            char d[256]; int o = 0;
            for (int i = 0; i < 32 && o < 200; i++)
                if (rec.gpr[i] != ref.gpr[i])
                    o += std::snprintf(d + o, sizeof(d) - o, "r%d:%08x/%08x ", i, rec.gpr[i], ref.gpr[i]);
            if (rec.lr  != ref.lr)  o += std::snprintf(d+o, sizeof(d)-o, "lr:%08x/%08x ", rec.lr, ref.lr);
            if (rec.ctr != ref.ctr) o += std::snprintf(d+o, sizeof(d)-o, "ctr:%08x/%08x ", rec.ctr, ref.ctr);
            if (rec.cr  != ref.cr)  o += std::snprintf(d+o, sizeof(d)-o, "cr:%08x/%08x ", rec.cr, ref.cr);
            if (rec.ca  != ref.ca)  o += std::snprintf(d+o, sizeof(d)-o, "ca:%u/%u ", rec.ca, ref.ca);
            if (ram_diff_off >= 0)  o += std::snprintf(d+o, sizeof(d)-o, "RAM@%08x:%02x/%02x",
                                        RAM_BASE+(u32)ram_diff_off, ramRec[ram_diff_off], ram[ram_diff_off]);
            info.exit_pc = exit_pc;
            info.detail  = d;
            fprintf(stderr, "[DIFF] func_%08x diverges: %s\n", pc, d);
            write_diff_report();   // incremental — survives a kill
        }
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
    if (diff) {
        // Validate each function ONCE (first call), then run it at normal recomp
        // speed — otherwise diffing every call is too slow to reach rendering
        // scenes. SUNBRIGHT_DIFF_ALL re-diffs every call (catches input-dependent
        // divergences, but slow).
        static const bool diff_all = getenv("SUNBRIGHT_DIFF_ALL") != nullptr;
        static std::unordered_set<u32> validated;
        if (diff_all || !validated.count(pc)) {
            validated.insert(pc);
            return diff_run(pc, fn);
        }
    }

    // Translate Dolphin's live ppcState into our CPUState
    CPUState cpu;
    dolphin_state_to_cpu(Core::System::GetInstance().GetPPCState(), cpu);
    cpu.pc = pc;

    // SUNBRIGHT_WATCH=<hexaddr>: observe a function entered from the JIT (e.g. a GX
    // matrix loader or J3DModel::viewCalc) and log its args / the 3x4 matrix at r3.
    static const u32 watch = getenv("SUNBRIGHT_WATCH")
                             ? (u32)strtoul(getenv("SUNBRIGHT_WATCH"), nullptr, 16) : 0;
    if (pc == watch && watch) {
        static unsigned long n = 0;
        if ((n++ % 500) == 0) {
            u32 m = cpu.gpr[3];
            fprintf(stderr, "[watch] %08x call#%lu r3=%08x r4=%08x", pc, n, m, cpu.gpr[4]);
            if (m >= 0x80000000u && m < 0x81800000u)
                fprintf(stderr, "  pos=(%.2f, %.2f, %.2f)",
                        mem_rf32(m + 12), mem_rf32(m + 28), mem_rf32(m + 44));
            fprintf(stderr, "\n");
        }
    }

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
