// ===========================================================================
// port/pal/base/ppc_arch_stub.cpp — PAL stub for the PowerPC architecture
// primitive PPCSync, referenced by the SMS audio DSP-channel path
// (JASDSPChannel) but with no native equivalent.
//
// SIGNATURE: from <dolphin/base/PPCArch.h> (extern "C"): void PPCSync(void).
//
// On the Gekko/Broadway CPU, `PPCSync` issues the PowerPC `sync` instruction —
// a heavyweight memory barrier that orders all loads/stores before subsequent
// ones (used to ensure DMA-visible writes are globally committed before the DSP
// reads them). On the PC host there is no GameCube DSP/DMA fabric to order
// against, and our compiler/host memory model already provides the ordering the
// single ported caller needs.
//
// BEHAVIOR (bring-up, but faithful): emit a full host memory fence so the
// semantic intent (a global ordering barrier) is preserved on x86-64 / arm64
// rather than silently dropped. This is correct, not a no-op: it is the closest
// portable analogue to `sync`. (It is not the no-op placeholder class.)
// ===========================================================================

#include <dolphin/base/PPCArch.h>

#include <atomic>

extern "C" {

void PPCSync() {
    // Full memory barrier — portable analogue of the PPC `sync` instruction.
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

}  // extern "C"
