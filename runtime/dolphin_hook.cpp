#include "dolphin_hook.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#ifdef HAVE_DOLPHIN_CORE
#  include "Core/System.h"
#endif

using RecompFunc = void (*)(CPUState&);
struct JumpEntry { uint32_t addr; RecompFunc fn; };

static void*  g_recomp_lib  = nullptr;
static JumpEntry* g_table   = nullptr;
static size_t g_table_size  = 0;
static std::unordered_map<uint32_t, RecompFunc> g_recomp_map;

bool dolphin_hook_install(const char* recomp_lib_path) {
    g_recomp_lib = dlopen(recomp_lib_path, RTLD_NOW | RTLD_LOCAL);
    if (!g_recomp_lib) {
        fprintf(stderr, "[sunbright] dlopen failed: %s\n", dlerror());
        return false;
    }

    g_table = (JumpEntry*)dlsym(g_recomp_lib, "g_recomp_table");
    size_t* table_sz = (size_t*)dlsym(g_recomp_lib, "g_recomp_table_size");

    if (!g_table || !table_sz) {
        fprintf(stderr, "[sunbright] recomp table not found in shared lib\n");
        dlclose(g_recomp_lib);
        g_recomp_lib = nullptr;
        return false;
    }

    g_table_size = *table_sz;
    for (size_t i = 0; i < g_table_size; i++)
        g_recomp_map[g_table[i].addr] = g_table[i].fn;

    fprintf(stderr, "[sunbright] Loaded %zu recompiled functions\n", g_table_size);

#ifdef HAVE_DOLPHIN_CORE
    // TODO: patch JitInterface::Compile to call recomp_lookup_and_call
    // This requires either:
    //  a) A Dolphin build with our hook point compiled in (preferred)
    //  b) Runtime function patching (fragile, platform-specific)
    // For now, Dolphin will call recomp_lookup_and_call via a compile-time hook.
    fprintf(stderr, "[sunbright] JIT hook: compile-time integration required\n");
#endif

    return true;
}

void dolphin_hook_uninstall() {
    g_recomp_map.clear();
    if (g_recomp_lib) { dlclose(g_recomp_lib); g_recomp_lib = nullptr; }
}

RecompFunc recomp_lookup(u32 address) {
    auto it = g_recomp_map.find(address);
    return (it != g_recomp_map.end()) ? it->second : nullptr;
}

void call_ppc(CPUState& cpu, u32 address) {
    RecompFunc fn = recomp_lookup(address);
    if (fn) {
        fn(cpu);
        return;
    }
#ifdef HAVE_DOLPHIN_CORE
    // Non-recompiled call: sync back to Dolphin state and let the JIT handle it.
    // Dolphin's JIT will pick up the PC from PowerPCState.pc via the dispatcher.
    auto& ppc = Core::System::GetInstance().GetPPCState();
    cpu_to_dolphin_state(cpu, ppc);
    ppc.pc = address;
    // The JIT dispatcher will continue execution from ppc.pc on return.
#else
    fprintf(stderr, "[sunbright] call_ppc 0x%08x: not recompiled and no JIT available\n", address);
#endif
}

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
    for (int i = 0; i < 8; i++) dst.spr[912 + i] = src.gqr[i];
}
#endif
