// OS interrupt primitives — native overrides.
//
// OSDisableInterrupts / OSEnableInterrupts / OSRestoreInterrupts are tiny
// mfmsr/mtmsr routines that toggle MSR[EE]. Because they write MSR, the
// recompiler routes them to Dolphin (function_needs_jit()), so they are *not*
// recompiled. OS code wraps every critical section in them — thousands of calls
// during boot alone.
//
// Why they need a native override (not JIT/interpreter):
//   In the C-call model a non-recomp call is run via run_jit_sync(), i.e. the
//   Dolphin *interpreter* SingleStep()'d until it returns to LR. SingleStep checks
//   and delivers external exceptions on EVERY step, so a pending interrupt can be
//   delivered *in the middle of OSDisableInterrupts* — after `mfmsr` has read
//   EE=1 but before `mtmsr` clears it — diverting to the exception vector inside a
//   primitive whose whole job is to make that impossible. The resulting state
//   corruption surfaced as an intermittent (~1/3 of runs) wild-pointer crash early
//   in boot. Running them as straight-line native code removes the per-instruction
//   interrupt window entirely.
//
// Interrupt-delivery semantics: we set MSR[EE] via msr_set_raw() (no synchronous
// CheckExceptions). In the C-call model recomp runs on the native C stack and never
// consults ppc.pc mid-tree, so an exception redirect here would be lost anyway;
// pending interrupts are delivered when the recomp call tree unwinds to the JIT
// boundary. Disable never delivers (EE goes to 0), so it is unaffected either way.

#include "../overrides.h"
#include "../intrinsics.h"

// Captured DSP-mail interrupts (aid_native.cpp) are flushed at EE re-enable — exactly when
// hardware would take the pended interrupt. Without this, an ack interrupt raised while the
// guest held EE off (the JAS mail engine masks around every send) waited for the poll_yield
// pump: the game blocks on the ack and writes nothing more, so the mailbox-write flush seam
// never fires → DSP render cycle collapsed to ~1/s (the slow-garbled-jingle bug, 2026-06-11).
void sunbright_dsp_flush_sync();   // aid_native.cpp (no-op when nothing captured / EE off)

namespace {

constexpr u32 MSR_EE = 0x00008000u;  // Gekko MSR external-interrupt-enable (bit 15)

// old EE as a 0/1 word, in the bit position the originals return (LSB). A blr is a
// plain C `return`, so an override just sets cpu state and returns to call_ppc.
inline u32 old_ee(u32 msr) { return (msr & MSR_EE) ? 1u : 0u; }

// 0x803458ac OSDisableInterrupts() -> previous EE
SUNBRIGHT_OVERRIDE(ov_OSDisableInterrupts, 0x803458acu) {
    u32 msr = msr_get();
    msr_set_raw(msr & ~MSR_EE);
    cpu.gpr[3] = old_ee(msr);
}

// 0x803458c0 OSEnableInterrupts() -> previous EE
SUNBRIGHT_OVERRIDE(ov_OSEnableInterrupts, 0x803458c0u) {
    u32 msr = msr_get();
    msr_set_raw(msr | MSR_EE);
    cpu.gpr[3] = old_ee(msr);
    if (!(msr & MSR_EE)) sunbright_dsp_flush_sync();   // EE 0->1: take pended DSP-mail IRQ now
}

// 0x803458d4 OSRestoreInterrupts(int level) -> previous EE
SUNBRIGHT_OVERRIDE(ov_OSRestoreInterrupts, 0x803458d4u) {
    u32 msr = msr_get();
    u32 nv  = (cpu.gpr[3] != 0) ? (msr | MSR_EE) : (msr & ~MSR_EE);
    msr_set_raw(nv);
    cpu.gpr[3] = old_ee(msr);
    if (!(msr & MSR_EE) && (nv & MSR_EE)) sunbright_dsp_flush_sync();  // EE 0->1
}

}  // namespace
