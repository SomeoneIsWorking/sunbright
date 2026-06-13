// JIT interception via linker --wrap on JitTrampoline's mangled symbol.
//
// This is the ONLY surviving --wrap seam. Every other Dolphin interception was converted to a
// direct fork hook (Common/SunbrightHooks.h sb_slot_*, installed by sb_install_hooks(),
// docs/re_notes/wrap_removal.md). JitTrampoline can't be: its definition (JitCommon/JitBase.cpp)
// and both call sites (Jit64/JitArm64 JitAsm.cpp) live entirely inside
// Source/Core/Core/PowerPC/, which the project forbids modifying. So it keeps --wrap.
//
// --wrap=_Z13JitTrampolineR7JitBasej instructs the linker to:
//   - Replace all references to _Z13JitTrampolineR7JitBasej with
//     __wrap__Z13JitTrampolineR7JitBasej  (our hook below)
//   - Rename the original definition to
//     __real__Z13JitTrampolineR7JitBasej  (Dolphin's original code)
//
// extern "C" prevents the compiler from further mangling the __wrap_/__real_ names.

#include <cstdio>
#include <cstdlib>
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "sunbright_bridge.h"

extern "C" void __real__Z13JitTrampolineR7JitBasej(JitBase& jit, u32 em_address);

// SUNBRIGHT_DBG_TRAMP=LO-HI (hex): log every Dolphin-JIT block dispatch whose address falls in
// [LO,HI), with the engine it routes to (recomp vs Dolphin JIT) and the live guest regs. Block-
// linking is off, so EVERY basic-block boundary reaches this trampoline — making this a precise
// window onto how guest control flows through a region and which engine runs each block. It traced
// the boot-logo r31 clobber to func_802f80d0 (endRendering): r31 entered correct (803e9700) and
// returned as the call's return address, so its recomp call tree drops the non-volatile r31.
static bool        g_dbg_tramp = false;
static u32         g_dbg_tramp_lo = 0, g_dbg_tramp_hi = 0;
static void dbg_tramp_init() {
    const char* e = getenv("SUNBRIGHT_DBG_TRAMP");
    if (!e) return;
    char* dash = nullptr;
    g_dbg_tramp_lo = (u32)strtoul(e, &dash, 16);
    g_dbg_tramp_hi = (dash && *dash == '-') ? (u32)strtoul(dash + 1, nullptr, 16) : g_dbg_tramp_lo + 4;
    g_dbg_tramp = true;
}

extern "C" void __wrap__Z13JitTrampolineR7JitBasej(JitBase& jit, u32 em_address) {
    const bool rc = SunbrightBridge::IsRecompiled(em_address);
    static const bool tramp_inited = (dbg_tramp_init(), true);
    (void)tramp_inited;
    if (g_dbg_tramp && em_address >= g_dbg_tramp_lo && em_address < g_dbg_tramp_hi) {
        auto& ppc = Core::System::GetInstance().GetPPCState();
        static long h = 0;
        if (h++ < 400)
            fprintf(stderr, "[tramp] dispatch %08x -> %-10s r31=%08x r3=%08x sp=%08x lr=%08x\n",
                    em_address, rc ? "recomp" : "DOLPHIN-JIT",
                    ppc.gpr[31], ppc.gpr[3], ppc.gpr[1], ppc.spr[SPR_LR]);
    }
    if (rc) {
        SunbrightBridge::Run(em_address);
        return;
    }
    __real__Z13JitTrampolineR7JitBasej(jit, em_address);
}
