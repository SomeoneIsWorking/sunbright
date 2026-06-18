#include "sunbright_bridge.h"
#include "watchdog.h"
#include "cpu_state.h"
#include "dolphin_hook.h"
#include "overrides.h"
#include "native_os.h"
#include "memory_bridge.h"
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <unordered_map>
#include <set>
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
void sunbright_trace_jit_entry(u32 address, u32 r3, u32 lr, u32 r4, u32 r5);   // dolphin_hook.cpp
struct JumpEntry { uint32_t addr; RecompFunc fn; };

extern f32 mem_rf32(u32 ea);  // from memory_bridge (for SUNBRIGHT_WATCH matrix dump)

// The recompiled function table is now linked directly into the sunbright binary
// (generated/jump_table.cpp), so there is no shared library to dlopen and no
// dynamic-symbol resolution to get wrong.
extern "C" const JumpEntry g_recomp_table[];
extern "C" const size_t    g_recomp_table_size;

static std::unordered_map<uint32_t, RecompFunc> g_table;
static bool g_inited = false;

// A/B switch (diagnostic): SUNBRIGHT_NORECOMP_NONOS=1 skips the nthr native scheduler (native_os) at
// the JIT-entry seam under no_recomp, to test letting Dolphin own threading. NOTE: this is INCOHERENT
// while recomp still runs (the unsolved Phase-B seam): recomp_lookup does NOT honor no_recomp, so
// recompiled OS code (reached via call_ppc super-calls) still calls native_os even with this set —
// the result is two schedulers over one context (the deadlock the IsRecompiled comment warns about).
// Keep it OPT-IN until recomp_lookup/recomp_raw honor no_recomp; default = native_os stays on.
static bool norecomp_skip_native_os() {
    static const bool no_recomp = getenv("SUNBRIGHT_NO_RECOMP") != nullptr;
    static const bool nonos     = getenv("SUNBRIGHT_NORECOMP_NONOS") != nullptr;
    return no_recomp && nonos;
}

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
    // True no-recomp / pure-JIT engine mode — checked FIRST, because it sets SUNBRIGHT_DISABLE_RECOMP
    // for the pure-JIT substrate (see main_sdl.cpp) and the plain `disabled` branch below must NOT
    // pre-empt it. Recomp is OFF (recomp_lookup/recomp_raw serve null, native_os off); the PC-native
    // engine mostly intercepts via PRE-HOOKS in the JIT trampoline (observe-then-let-Dolphin-JIT-the-
    // original). The exception is FULL-REPLACEMENT overrides explicitly marked purejit-safe (fastboot
    // app-state returns, ngx native draw): those still dispatch via Run() (run native, return to lr —
    // Dolphin JIT continues). Everything else falls to Dolphin's JIT. See overrides.h.
    if (sunbright_purejit_mode()) return override_is_purejit_safe(pc);
    static const bool disabled = getenv("SUNBRIGHT_DISABLE_RECOMP") != nullptr;
    if (disabled) return false;
    if (is_jit_forced(pc)) return false;            // routed to Dolphin's JIT
    // NO_RECOMP boot-hang bisection (2026-06-18): SUNBRIGHT_NORECOMP_NOOV=1 skips ALL native
    // overrides + native-OS under no_recomp → should behave like DISABLE_RECOMP and boot, proving
    // the hang is in the (hybrid-era) engine overrides. Refine to a group/range kill-switch next.
    static const bool no_overrides = getenv("SUNBRIGHT_NORECOMP_NOOV") != nullptr;
    if (no_overrides) return false;
    if (override_lookup(pc)) return true;           // hand-written native override
    // NO_RECOMP: native-OS (nthr scheduler) is an EXECUTION-MODEL override built for the recomp
    // hybrid. Under pure Dolphin JIT it deadlocks boot — nthr parks every guest thread waiting for
    // tokens the recomp call model used to grant, then the idle driver spins forever (no thread
    // runnable). Under no_recomp Dolphin must own threading (exactly as DISABLE_RECOMP does, which
    // boots). SUNBRIGHT_NORECOMP_KEEPOS=1 forces the old behavior for A/B.
    if (!norecomp_skip_native_os() && native_os_lookup(pc)) return true;  // native-OS primitive — MUST
        // intercept on the JIT-entry path too, else JIT'd code (e.g. mountStageArchive) calling
        // OSSleepThread links straight to the recompiled GC scheduler, bypassing nthr → two
        // schedulers over one context → corrupted thread state (the mountFixed null-this crash).
    // NO-RECOMP / JIT-native mode (2026-06-18 arch pivot, memory no-recomp-jit-native-direction):
    // the static recompiler is being removed from the game — gameplay logic runs under Dolphin's
    // JIT while the PC-native ENGINE (overrides + native-OS above) still intercepts. Distinct from
    // DISABLE_RECOMP, which returns false BEFORE the override/native-OS checks (pure-Dolphin
    // oracle, no native engine). The recomp table is still LINKED in this phase so override
    // super-calls (recomp_raw) keep working until the super-call seam is converted (Phase B).
    static const bool no_recomp = getenv("SUNBRIGHT_NO_RECOMP") != nullptr;
    if (no_recomp) return false;                    // gameplay → Dolphin JIT
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
    // FPRs (raw bits, both paired-single slots) + FPSCR. The recomp run clobbers
    // these on ppc; without restoring them the interpreter reconstruction reads the
    // recomp's float results as its *inputs*, so every FP-heavy function (sinf/powf/
    // matan/GXSetFog/audio) falsely diverges on the downstream integer/CR/CA bits.
    u64 ps0[32], ps1[32]; u32 fpscr;
};
void snap(const PowerPC::PowerPCState& s, RegSnap& r) {
    for (int i = 0; i < 32; i++) r.gpr[i] = s.gpr[i];
    for (int i = 0; i < 32; i++) { r.ps0[i] = s.ps[i].PS0AsU64(); r.ps1[i] = s.ps[i].PS1AsU64(); }
    r.lr = s.spr[SPR_LR]; r.ctr = s.spr[SPR_CTR];
    // XER must be captured/restored in full — SO/OV too, not just CA — or the
    // interpreter run starts with a stale XER[SO] and every cmp (which copies
    // XER[SO] into the CR) falsely "diverges".
    r.cr = s.cr.Get(); r.ca = s.xer_ca; r.so_ov = s.xer_so_ov; r.pc = s.pc;
    r.fpscr = s.fpscr.Hex;
}
void restore(PowerPC::PowerPCState& s, const RegSnap& r) {
    for (int i = 0; i < 32; i++) s.gpr[i] = r.gpr[i];
    for (int i = 0; i < 32; i++) { s.ps[i].SetPS0(r.ps0[i]); s.ps[i].SetPS1(r.ps1[i]); }
    s.xer_so_ov = r.so_ov;
    s.spr[SPR_LR] = r.lr; s.spr[SPR_CTR] = r.ctr;
    s.cr.Set(r.cr); s.xer_ca = r.ca; s.pc = r.pc;
    s.fpscr.Hex = r.fpscr;
}

// ── Correctness harness ──────────────────────────────────────────────────────
// Aggregate every recomp function that diverges from Dolphin's interpreter across
// a play session into a prioritized, named report (instead of stopping at the
// first). Run with SUNBRIGHT_DIFF=1; the report is rewritten incrementally to
// scratch/sunbright_diff_report.txt (override with SUNBRIGHT_DIFF_REPORT) so results
// survive a kill. NEVER default this to /tmp — it's a small RAM-backed tmpfs with a
// per-user quota; large run artifacts belong under the repo's gitignored scratch/.
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
    const char* env = std::getenv("SUNBRIGHT_DIFF_REPORT");
    std::string path = (env && *env) ? env : "scratch/sunbright_diff_report.txt";
    if (auto p = std::filesystem::path(path).parent_path(); !p.empty())
        std::filesystem::create_directories(p);
    std::ofstream f(path);
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
    g_recomp_context_switched = false;
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
    // fn runs the whole call tree on the C stack. It may end by a C `return`
    // (top-level blr) OR by a tail-branch into non-recomp code that siglongjmps out.
    // Give it a tail-jmp target so the latter lands here cleanly.
    bool tailed = false;
    {
        sigjmp_buf tjb;
        sigjmp_buf* prev = sunbright_set_tail_jmp(&tjb);
        if (sigsetjmp(tjb, 0) == 0) fn(cpu);
        else                        tailed = true;   // tail_ppc already committed ppc.pc
        sunbright_set_tail_jmp(prev);
    }
    g_in_recomp = 0;
    // On a normal C return nothing set ppc.pc, so commit cpu→ppc and use the return
    // address as the exit PC; on a tail-branch exit, tail_ppc already committed both.
    if (!tailed) { cpu_to_dolphin_state(cpu, ppc); ppc.pc = cpu.lr; }
    const bool mmio = g_recomp_touched_mmio;
    RegSnap rec; snap(ppc, rec);
    const u32 exit_pc = ppc.pc;
    std::memcpy(ramRec.data(), ram, RAM_SIZE);   // recomp's resulting RAM

    // Hardware-register reads legitimately differ between two separate runs (the
    // register changes with time). A context-switch yield (OSLoadContext handoff) means
    // the function rescheduled rather than returned, so there is no comparable exit
    // state. Don't compare either — commit recomp and move on.
    if (mmio || g_recomp_context_switched) {
        restore(ppc, rec); std::memcpy(ram, ramRec.data(), RAM_SIZE); return true;
    }

    // (2) reference: interpreter from the same entry until the function RETURNS.
    //
    // The exit must be detected by ground truth, NOT by `ppc.pc == exit_pc` where
    // exit_pc=cpu.lr-after-the-recomp-run. When a function's call tree runs non-recomp
    // code via call_ppc (e.g. OSLockMutex, which has mtmsr → JIT), the interpreter
    // excursion mutates LR, so the recomp can finish with cpu.lr pointing INSIDE an OS
    // primitive (0x80346778 in OSLockMutex, 0x803506e8, …). The reference loop would
    // then stop the FIRST time the interpreter passes through that mid-primitive address
    // — long before the function actually returns — leaving registers half-set (the
    // notorious r4/r5 ≈ 0x2030 sentinel) and reporting a flood of bogus divergences.
    //
    // A normal function returns to its ENTRY LR (s0.lr) with the stack frame popped back
    // to the entry SP (s0.gpr[1]); s0.lr lives in the *caller*, so the body never reaches
    // it except by returning, and the SP guard rejects same-address pass-throughs at a
    // deeper frame. For a tail-branch exit the recomp committed a real target PC + SP, so
    // match those.
    restore(ppc, s0);
    std::memcpy(ram, ram0.data(), RAM_SIZE);
    ppc.pc = pc; ppc.npc = pc;
    auto& interp = sys.GetInterpreter();
    long steps = 0; constexpr long MAX = 3'000'000;
    const u32 ret_pc = tailed ? exit_pc : s0.lr;
    const u32 ret_sp = tailed ? rec.gpr[1] : s0.gpr[1];
    while (!(ppc.pc == ret_pc && ppc.gpr[1] == ret_sp) && steps++ < MAX)
        interp.SingleStepInner();

    if (steps >= MAX) {
        // The interpreter never reached recomp's exit within budget — almost always
        // a long internal loop (cache flush, memset) recomp finished in native C.
        // Inconclusive, not a divergence: commit recomp's result and move on.
        // SUNBRIGHT_DIFF_LOG_SKIP logs these so we can tell whether a suspected
        // corruptor (e.g. an SZS/Yaz0 decompressor) is being silently skipped here.
        static const bool log_skip = getenv("SUNBRIGHT_DIFF_LOG_SKIP") != nullptr;
        if (log_skip) {
            static std::set<u32> seen;
            if (seen.insert(pc).second)
                fprintf(stderr, "[DIFF-SKIP] func_%08x (long-loop, %ldM steps, exit=%08x)\n",
                        pc, MAX / 1'000'000, exit_pc);
        }
        restore(ppc, rec);
        std::memcpy(ram, ramRec.data(), RAM_SIZE);
        return true;
    }

    RegSnap ref; snap(ppc, ref);   // committed state is the interpreter's = correct

    // (3) compare — but only the ABI-OBSERVABLE state at a return boundary.
    //
    // At a function boundary the PPC EABI only constrains: the return value (r3), and the
    // CALLEE-PRESERVED registers (non-volatile GPRs r14-r31 and CR fields CR2-CR4). The
    // volatile/scratch set — r0, r4-r12, LR, CTR, XER[CA], and the volatile CR fields
    // (CR0/1/5/6/7) — is "undefined" across the call: the caller must assume it's
    // clobbered, so the recomp and the interpreter are FREE to leave different values
    // there without it being a bug. Comparing those produced a flood of false positives
    // (the notorious r4/r5 ≈ 0x2030 sentinels: a shared epilogue's intermediate scratch),
    // drowning the real signal. Compare only what a correct callee must guarantee — plus
    // memory (DIFF_RAM), which is where a genuine corruptor bug actually lands.
    constexpr u32 CR_NONVOL = 0x00FFF000u;   // CR2,CR3,CR4 (callee-saved); CR0/1/5/6/7 volatile
    bool regs_differ = (rec.gpr[3] != ref.gpr[3]) ||
                       ((rec.cr & CR_NONVOL) != (ref.cr & CR_NONVOL));
    for (int i = 14; i < 32 && !regs_differ; i++)
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
            // Only the ABI-observable registers (matches the comparison above): the
            // return value r3 and the non-volatile r14-r31 + CR2-CR4.
            char d[256]; int o = 0;
            if (rec.gpr[3] != ref.gpr[3])
                o += std::snprintf(d + o, sizeof(d) - o, "r3:%08x/%08x ", rec.gpr[3], ref.gpr[3]);
            for (int i = 14; i < 32 && o < 200; i++)
                if (rec.gpr[i] != ref.gpr[i])
                    o += std::snprintf(d + o, sizeof(d) - o, "r%d:%08x/%08x ", i, rec.gpr[i], ref.gpr[i]);
            if ((rec.cr & CR_NONVOL) != (ref.cr & CR_NONVOL))
                o += std::snprintf(d+o, sizeof(d)-o, "cr:%08x/%08x ", rec.cr, ref.cr);
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
#ifdef HAVE_DOLPHIN_CORE
    // First guest entry on the EmuThread: adopt it as nthr guest thread 0 (holds the CPU
    // token). Idempotent (std::call_once) and inert with only thread 0 active.
    sunbright_adopt_cpu_thread();
    watchdog_register_emu_thread();   // record this (the emu thread) for the freeze-backtrace signal
#endif
    // Native-OS / PC-native subsystem override takes precedence even at a top-level JIT entry, so
    // the seam is universal (call_ppc + run_jit_sync interpreter + here). Runs and returns to lr.
#ifdef HAVE_DOLPHIN_CORE
    if (NativeOSFn nf = norecomp_skip_native_os() ? nullptr : native_os_lookup(pc)) {
        CPUState cpu; dolphin_state_to_cpu(Core::System::GetInstance().GetPPCState(), cpu); cpu.pc = pc;
        nf(cpu);
        auto& ppc = Core::System::GetInstance().GetPPCState();
        cpu_to_dolphin_state(cpu, ppc);
        ppc.pc = ppc.npc = cpu.lr;
        return true;
    }
#endif
    // Hand-written native override takes precedence over the generated function.
    RecompFunc fn = override_lookup(pc);
    if (!fn) {
        auto it = g_table.find(pc);
        if (it == g_table.end()) return false;
        fn = it->second;
    }
#ifdef HAVE_DOLPHIN_CORE
    // JIT-entry visibility for the call tracers: a JIT-context caller enters recompiled
    // functions HERE, bypassing call_ppc — without this, traced functions invoked from
    // JIT-handed-off code report zero calls (the choppy-music false trail, 2026-06-11).
    {
        auto& tppc = Core::System::GetInstance().GetPPCState();
        ::sunbright_trace_jit_entry(pc, tppc.gpr[3], LR(tppc), tppc.gpr[4], tppc.gpr[5]);
    }
#endif

#ifdef HAVE_DOLPHIN_CORE
    static const bool diff = getenv("SUNBRIGHT_DIFF") != nullptr;
    if (diff) {
        // Validate each function ONCE (first call), then run it at normal recomp
        // speed — otherwise diffing every call is too slow to reach rendering
        // scenes. SUNBRIGHT_DIFF_ALL re-diffs every call (catches input-dependent
        // divergences, but slow).
        static const bool diff_all = getenv("SUNBRIGHT_DIFF_ALL") != nullptr;
        // SUNBRIGHT_DIFF_ONLY=<hexaddr>: re-diff just this one pc on EVERY call (and skip
        // diffing all others) — fast enough to reach input-dependent divergences that the
        // once-per-pc validation misses (a function whose first call has benign inputs).
        static const u32 diff_only = getenv("SUNBRIGHT_DIFF_ONLY")
                                     ? (u32)strtoul(getenv("SUNBRIGHT_DIFF_ONLY"), nullptr, 16) : 0;
        static std::unordered_set<u32> validated;
        if (diff_only) {
            if (pc == diff_only) return diff_run(pc, fn);
        } else if (diff_all || !validated.count(pc)) {
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
            fprintf(stderr, "[watch] %08x call#%lu lr=%08x r3=%08x r4=%08x", pc, n, cpu.lr, m, cpu.gpr[4]);
            if (m >= 0x80000000u && m < 0x81800000u)
                fprintf(stderr, "  pos=(%.2f, %.2f, %.2f)",
                        mem_rf32(m + 12), mem_rf32(m + 28), mem_rf32(m + 44));
            fprintf(stderr, "\n");
        }
    }

    // Call the native function. It runs the whole call tree on the native C stack and
    // can exit two ways:
    //  (a) the top-level function returns via a C `return` (its blr) — nothing set
    //      ppc.pc, so commit our state and continue the JIT at cpu.lr;
    //  (b) a tail-branch into non-recomp code siglongjmp'd back here having already
    //      committed ppc.pc — just fall through to the JIT.
    static const bool trace = getenv("SUNBRIGHT_TRACE") != nullptr;
    if (trace) fprintf(stderr, "[recomp] running func_%08x\n", pc);
    sunbright_dispatch_profile_note(pc);  // SUNBRIGHT_DISPATCH_PROFILE: which blocks bounce the boundary
    sunbright_run_recomp_tree(cpu, fn);   // shared entry/tail-jmp logic (see dolphin_hook.cpp)
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
