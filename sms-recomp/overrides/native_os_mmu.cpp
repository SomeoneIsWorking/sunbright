// native_os_mmu.cpp — memory-protection / MMU overrides.
//
// These functions configure GameCube memory-protection hardware. Beyond having no host
// counterpart, they are structurally un-recompilable by a function-granular recompiler:
// the sequence is
//
//     rlwinm rN,rN,0,2,31   ; virtual -> physical
//     mtsrr0 rN
//     mfmsr / rlwinm        ; clear MSR[IR|DR] (translation off)
//     mtsrr1 / rfi          ; jump to the physical address
//     ... work with the MMU off ...
//     rfi                   ; back to LR, translation on again
//
// The return rfi lands MID-FUNCTION (the instruction after the bl that got here), and a
// recompiler that emits one C function per guest function has no way to resume at an
// arbitrary interior address. Recompiling this correctly would require basic-block
// granularity for the whole image to serve one boot-time hardware routine.

#include "overrides.h"

#include <lucent/log.h>

namespace {

// __OSInitMemoryProtection @ 0x803465b8 — called once from OSInit.
//
// Retail: enables the memory-protection interface so writes into protected ranges raise
// an interrupt, used to catch stray DMA and null writes during development.
//
// Native: the port already has a stronger version of exactly this guarantee. rt_mem_init
// maps 32 MB and mprotect(PROT_NONE)s everything past the 24 MB of real MEM1, so a stray
// guest access faults immediately on the host instead of silently aliasing. There is
// nothing left for this function to arrange.
void os_init_memory_protection(CPUState& cpu) {
    (void)cpu;
}

} // namespace

SB_OVERRIDE(0x803465b8u, os_init_memory_protection, "__OSInitMemoryProtection",
            "GC memory-protection HW has no host counterpart; the runtime's mprotect "
            "poison already traps stray accesses")
