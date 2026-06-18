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
#include "overrides.h"
#include "dolphin_hook.h"   // dolphin_state_to_cpu

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

// ── Engine pre-hook seam (no-recomp / pure-JIT mode) ─────────────────────────
// A pre-hook OBSERVES a guest block (reads args / guest RAM to feed the PC-native engine, e.g. ngx
// J3D capture) off a CPUState mirrored from Dolphin's live PPC state, then control falls through so
// Dolphin JITs the ORIGINAL block (whose sub-calls re-consult the trampoline naturally). This is the
// engine seam under SUNBRIGHT_PUREJIT, where recomp is off and overrides can't dispatch via Run()
// (see overrides.h sunbright_purejit_mode). Cheap-rejected to zero cost when no pre-hook is
// registered (the recomp path). Observation only — a pre-hook must not change control flow.
static bool run_prehook(u32 em_address) {
    static const bool any = prehooks_registered();   // skip the lookup entirely when none registered
    if (!any) return false;
    PreHookFn fn = prehook_lookup(em_address);
    if (!fn) return false;
    CPUState cpu;
    dolphin_state_to_cpu(Core::System::GetInstance().GetPPCState(), cpu);
    cpu.pc = em_address;
    fn(cpu);
    return true;
}

// The fork hook body. jit is JitBase* (opaque here — we never need it; non-recomp returns false
// and the fork's JitTrampoline calls jit.Jit() itself). Returns true iff we ran a recompiled block.
extern "C" bool sb_hook_jit_trampoline(void* /*jit*/, u32 em_address) {
    run_prehook(em_address);   // engine observe seam (no-op when no pre-hook registered)
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
