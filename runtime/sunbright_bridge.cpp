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
#  include "Core/System.h"
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

bool Run(uint32_t pc) {
    // Hand-written native override takes precedence over the generated function.
    RecompFunc fn = override_lookup(pc);
    if (!fn) {
        auto it = g_table.find(pc);
        if (it == g_table.end()) return false;
        fn = it->second;
    }

#ifdef HAVE_DOLPHIN_CORE
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
