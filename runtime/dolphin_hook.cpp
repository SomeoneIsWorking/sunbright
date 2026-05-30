#include "dolphin_hook.h"
#include "overrides.h"
#include <dlfcn.h>
#include <cstdlib>
#ifdef HAVE_DOLPHIN_CORE
#  include "Core/HW/SystemTimers.h"
#endif
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
    // Addresses we deliberately leave to Dolphin's JIT resolve as "not recompiled".
    if (is_jit_forced(address)) return nullptr;
    // Hand-written native overrides win over the generated function.
    if (RecompFunc ov = override_lookup(address)) return ov;
    auto it = g_recomp_map.find(address);
    return (it != g_recomp_map.end()) ? it->second : nullptr;
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
        fn(cpu);
        return;
    }
#ifdef HAVE_DOLPHIN_CORE
    // Non-recompiled call: sync back to Dolphin state and let the JIT handle it.
    // Dolphin's JIT will pick up the PC from PowerPCState.pc via the dispatcher.
    static u32 last_non_recomp = 0;
    if (address != last_non_recomp) {
        fprintf(stderr, "[call_ppc] non-recomp exit to 0x%08x\n", address);
        last_non_recomp = address;
    }
    auto& ppc = Core::System::GetInstance().GetPPCState();
    cpu_to_dolphin_state(cpu, ppc);
    ppc.pc = address;
    // The JIT dispatcher will continue execution from ppc.pc on return.
#else
    fprintf(stderr, "[sunbright] call_ppc 0x%08x: not recompiled and no JIT available\n", address);
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
    for (int i = 0; i < 8; i++) dst.spr[912 + i] = src.gqr[i];
}
#endif
