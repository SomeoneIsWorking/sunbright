// ===========================================================================
// port/pal/audio/ai_ar_dsp_stub.cpp — PAL stubs for the GameCube low-level
// audio hardware SDK: AI (audio interface DMA), AR/ARQ (ARAM allocator + DMA
// queue), the DSP mailbox, and the JSystem DSP-task layer (Dsp* / dspproc /
// dsptask). These are the audio-hardware-boundary symbols the SMS JAudio stack
// (JASystem) references but that have no native equivalent: on the PC port,
// audio is synthesized/mixed natively (see runtime/native_jas.cpp in the
// recompiler track), not through ARAM + the GameCube DSP ucode.
//
// SYMBOLS RESOLVED:
//   AI  : AIRegisterDMACallback, AISetDSPSampleRate, AIStartDMA
//   AR  : ARInit, ARAlloc, ARGetBaseAddress, ARGetSize
//   ARQ : ARQInit, ARQPostRequest
//   DSP : DSPCheckMailFromDSP, DSPReadMailFromDSP   (<dolphin/dsp.h>, extern C)
//   DSP-task layer (C++ mangled, via the shadow <JSystem/dspproc.h> /
//                   <JSystem/dsptask.h> — uintptr_t-widened, see those headers):
//         DsetupTable, DsetMixerLevel, DsyncFrame2, DspBoot, DspFinishWork,
//         DSPReleaseHalt
//
// SIGNATURES: the AI/AR/ARQ/DSP-mail ones come from the extern "C" dolphin SDK
// headers. The Dsp* ones come from the shadow JSystem headers — including those
// (rather than re-typing) makes the C++ mangling match the caller's exactly
// (the shadow widened the buffer-address args to uintptr_t for the 64-bit host;
// see the shadow header comments, which explicitly mark these as PAL items).
//
// BEHAVIOR (bring-up placeholders — FLAG FOR PM): all are no-ops / sane returns.
// No real ARAM, no DSP ucode, no AI DMA. The native audio engine owns sound on
// the PC port; these exist purely so the JAudio engine TUs LINK. Specifically:
//   - AIRegisterDMACallback returns nullptr (no previous callback registered).
//   - AR allocator: ARInit returns 0; ARAlloc returns 0 (a 0 ARAM "address" —
//     the engine treats ARAM offsets as opaque u32 handles, and with native
//     synthesis nothing dereferences them as real ARAM); ARGetBaseAddress/
//     ARGetSize return 0 (no ARAM region). FLAG: if any ported engine code
//     actually DMAs to/from these handles it will misbehave — needs a real
//     host-RAM-backed ARAM shim then.
//   - DSP mailbox: DSPCheckMailFromDSP returns 0 (no mail pending),
//     DSPReadMailFromDSP returns 0 (empty mail). The native engine does not run
//     the GC DSP ucode handshake.
//   - All void Dsp*/Dset*/Dsync* / DspBoot / DspFinishWork: no-ops.
// ===========================================================================

#include <dolphin/ai.h>
#include <dolphin/ar.h>
#include <dolphin/dsp.h>

#include <JSystem/dspproc.h>  // shadow: DsetupTable(u32,uintptr_t x4) etc.
#include <JSystem/dsptask.h>  // shadow: DsyncFrame2(u32,uintptr_t,uintptr_t), DspBoot, DspFinishWork

// ------------------------------------------------------------------ AI / AR / ARQ / DSP mail (extern "C")
extern "C" {

// AI (audio interface)
AIDCallback AIRegisterDMACallback(AIDCallback /*callback*/) {
    return nullptr;  // no previously-registered callback
}
void AISetDSPSampleRate(u32 /*rate*/) {}
void AIStartDMA(void) {}

// AR / ARAM allocator
u32 ARInit(u32* /*stack_index_addr*/, u32 /*num_entries*/) { return 0; }
u32 ARAlloc(u32 /*length*/) { return 0; }   // opaque ARAM handle; see header note
u32 ARGetBaseAddress(void) { return 0; }
u32 ARGetSize(void) { return 0; }            // no ARAM region on the PC port

// ARQ (ARAM DMA request queue)
void ARQInit(void) {}
// source/dest widened to uintptr_t by the shadow <dolphin/ar.h> (host addrs).
void ARQPostRequest(struct ARQRequest* /*request*/, u32 /*owner*/, u32 /*type*/,
                    u32 /*priority*/, uintptr_t /*source*/, uintptr_t /*dest*/,
                    u32 /*length*/, ARQCallback /*callback*/) {
    // No ARAM DMA. Do NOT invoke the callback: nothing was transferred, and an
    // out-of-band callback on an arbitrary thread could corrupt engine state.
}

// DSP mailbox
u32 DSPCheckMailFromDSP(void) { return 0; }  // no mail pending
u32 DSPReadMailFromDSP(void) { return 0; }   // empty mail

}  // extern "C"

// ------------------------------------------------------------------ JSystem DSP-task layer (C++ mangled)
// NOT extern "C": these match the shadow-header C++ signatures so their mangled
// names (e.g. _Z11DsetupTablejmmmm) equal the caller's references.

void DsetupTable(u32 /*subframes*/, uintptr_t /*ch_buf*/,
                 uintptr_t /*res_filter*/, uintptr_t /*adpcm_filter*/,
                 uintptr_t /*fx_buf*/) {}
void DsetMixerLevel(float /*level*/) {}
void DsyncFrame2(u32 /*subframes*/, uintptr_t /*buf_start*/,
                 uintptr_t /*buf_end*/) {}
void DspBoot(void (* /*entry*/)(void*)) {}
void DspFinishWork(u16 /*intcount*/) {}
void DSPReleaseHalt() {}
