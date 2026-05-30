#include "sunbright_bridge.h"
#include "cpu_state.h"
#include "dolphin_hook.h"
#include <dlfcn.h>
#include <cstdio>
#include <unordered_map>
#include <string>

// Pull in Dolphin's PowerPCState when building inside Dolphin
#ifdef HAVE_DOLPHIN_CORE
#  include "Core/PowerPC/PowerPC.h"
#  include "Core/System.h"
#endif

using RecompFunc = void (*)(CPUState&);
struct JumpEntry { uint32_t addr; RecompFunc fn; };

static void* g_lib = nullptr;
static std::unordered_map<uint32_t, RecompFunc> g_table;
static bool g_inited = false;

namespace SunbrightBridge {

bool Init(const char* lib_path) {
    if (g_inited) return true;

    g_lib = dlopen(lib_path, RTLD_NOW | RTLD_GLOBAL);
    if (!g_lib) {
        fprintf(stderr, "[sunbright] dlopen(%s): %s\n", lib_path, dlerror());
        return false;
    }

    auto* table = (const JumpEntry*)dlsym(g_lib, "g_recomp_table");
    auto* size  = (const size_t*)dlsym(g_lib, "g_recomp_table_size");
    if (!table || !size) {
        fprintf(stderr, "[sunbright] recomp table symbols not found\n");
        dlclose(g_lib); g_lib = nullptr;
        return false;
    }

    for (size_t i = 0; i < *size; i++)
        g_table[table[i].addr] = table[i].fn;

    fprintf(stderr, "[sunbright] Loaded %zu recompiled functions from %s\n", *size, lib_path);
    g_inited = true;
    return true;
}

bool IsRecompiled(uint32_t pc) {
    return g_inited && g_table.count(pc);
}

bool Run(uint32_t pc) {
    auto it = g_table.find(pc);
    if (it == g_table.end()) return false;

#ifdef HAVE_DOLPHIN_CORE
    // Translate Dolphin's live ppcState into our CPUState
    CPUState cpu;
    dolphin_state_to_cpu(Core::System::GetInstance().GetPPCState(), cpu);
    cpu.pc = pc;

    // Call the native function
    it->second(cpu);

    // Write back
    cpu_to_dolphin_state(cpu, Core::System::GetInstance().GetPPCState());
    // Update Dolphin's PC to wherever the function ended (LR or next instruction)
    // The recompiled function already set cpu.lr on bl, and returned on blr.
    // Dolphin's dispatcher will continue from ppcState.pc.
    Core::System::GetInstance().GetPPCState().pc = cpu.lr;
#else
    fprintf(stderr, "[sunbright] Run() called without HAVE_DOLPHIN_CORE — no-op\n");
    (void)it;
#endif
    return true;
}

void Shutdown() {
    g_table.clear();
    if (g_lib) { dlclose(g_lib); g_lib = nullptr; }
    g_inited = false;
}

}  // namespace SunbrightBridge
