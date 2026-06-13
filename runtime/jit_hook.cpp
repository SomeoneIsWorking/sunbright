// JIT interception via the Dolphin-fork JitTrampoline hook slot (Common/SunbrightHooks.h
// sb_slot_jit_trampoline). This was the last surviving linker --wrap seam; it is now a direct fork
// hook like every other interception (docs/re_notes/wrap_removal.md), because ld64 (macOS/arm64)
// cannot do --wrap. The fork's JitTrampoline (JitCommon/JitBase.cpp) calls our hook first: if we
// ran a recompiled block we return true (Dolphin skips its JIT); otherwise we return false and
// Dolphin JITs the block as normal. Installed by sb_install_hooks() (runtime/sunbright_hooks.cpp).
//
// When the slot is null (offline tools / a standalone fork build), JitTrampoline runs Dolphin's
// JIT unchanged — so the fork still works standalone.

#include <cstdio>
#include <cstdlib>
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "sunbright_bridge.h"

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

// The fork hook body. jit is JitBase* (opaque here — we never need it; non-recomp returns false
// and the fork's JitTrampoline calls jit.Jit() itself). Returns true iff we ran a recompiled block.
extern "C" bool sb_hook_jit_trampoline(void* /*jit*/, u32 em_address) {
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
        return true;
    }
    return false;
}
