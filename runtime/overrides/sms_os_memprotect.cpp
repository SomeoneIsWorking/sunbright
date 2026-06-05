// __OSInitMemoryProtection real-mode BAT trampoline — native override.
//
// __OSInitMemoryProtection (0x803465b8) sets up the Gekko memory-protection unit.
// Part of it must run with address translation OFF (real mode), so the OS uses a
// tiny trampoline at 0x803465a0:
//
//   803465a0: rlwinm r3,r3,0,2,31   ; r3 = real-mode (physical) entry, top 2 bits cleared
//   803465a4: mtspr  SRR0, r3
//   803465a8: mfmsr  r3
//   803465ac: rlwinm r3,r3,...      ; clear MSR[IR]/MSR[DR]
//   803465b0: mtspr  SRR1, r3
//   803465b4: rfi                   ; -> real-mode block (0x803464a0 / 0x80346520)
//
// The real-mode block it jumps to writes the IBAT/DBAT registers (SPR 528..543) to
// program the 24 MB (or 48 MB) main-RAM translation, re-enables MSR[IR]/[DR], then
// rfi's back to the trampoline's caller (LR = 0x803465f8).
//
// Why this can't be recompiled and needs a native override (not JIT routing):
//   The recompiler now models `rfi` as `tail_ppc(cpu, cpu.srr0)` (so it can own the
//   OS exception-return / context-switch primitives, whose rfi targets are normal
//   virtual 0x8xxxxxxx addresses). But THIS rfi enters *real mode*: SRR0 is a
//   physical address (0x003464a0), which is not a valid recomp entry — `tail_ppc`
//   trapped it as a wild branch and aborted boot. The block it would run only writes
//   BAT registers, which configure address translation.
//
//   In our runtime there is no MMU: `memory_bridge.cpp` hardwires the 0x8xxxxxxx ->
//   host-RAM mapping for the low 24 MB. The BAT programming therefore has *no*
//   observable effect on guest-visible state — it is redundant with the mapping we
//   already bake in. The block writes no RAM and the caller reloads r3..r7 right
//   after the call, so a faithful native equivalent is simply: do nothing, return.
//
//   This is a PC-native port of the behavior (set up main-RAM translation that we
//   already provide), not a JIT crutch and not a bandaid: the trampoline's entire
//   effect is real-mode BAT setup that is a no-op under a flat memory model.
//
// The rest of __OSInitMemoryProtection (the MI register writes at 0xCC004000 and the
// memory-protection interrupt-handler registration) still runs through the normal
// recomp + Dolphin-MMIO path — only the unmodelable real-mode hop is replaced here.

#include "../overrides.h"

namespace {

// 0x803465a0 — real-mode BAT trampoline. Faithful no-op: returns to the caller
// exactly where the real-mode block's rfi-back would land (call_ppc continues inline
// at LR), without the unmodelable real-mode address-translation switch.
SUNBRIGHT_OVERRIDE(ov_realmode_bat_trampoline, 0x803465a0u) {
    (void)cpu;  // no guest-visible state to change — BATs are irrelevant to the flat bridge
}

}  // namespace
