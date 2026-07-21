// native_dsp.cpp — DSP interface overrides.
//
// The GameCube DSP is a separate 16-bit DSP core with its own microcode, mailboxes and
// reset handshake. The retail boot uploads an init microcode and then polls DSPCR
// (0xCC00500A) until the DSP acknowledges. This port has no DSP and will never have one:
// audio is host-side, so the register handshake has no counterpart to complete it and the
// poll spins forever (3.9 M reads of 0xCC00500A per second, observed).
//
// Emulating the handshake would mean emulating the DSP core, which is the opposite of the
// port's direction. The seam is therefore the SDK function, not the register.

#include "overrides.h"

#include <lucent/log.h>

namespace {

// __OSInitAudioSystem @ 0x803433b4 — called once from OSInit.
//
// Retail: resets the DSP, uploads init ucode, waits for the mailbox handshake, and
// registers the AI DMA callback. Native: nothing to do. It returns void and OSInit
// ignores it, so there is no return value to fabricate — the guest-visible state it
// leaves behind is DSP hardware state, which nothing in a host-audio port reads.
//
// NOTE: this covers DSP *init* only. When the JAS audio arc is wired up, the ARAM and AI
// paths get their own overrides; this one stays a no-op regardless.
void os_init_audio_system(CPUState& cpu) {
    (void)cpu;
}

// __DSP_boot_task @0x8035406c — uploads the DSP microcode and handshakes with the DSP over
// the mailbox registers:
//
//   loop: bl __DSPCheckMailToDSP ; cmplwi r3,0 ; beq loop      ; wait for the DSP to answer
//         bl __DSPSendMailToDSP  ; bl __DSPCheckMailFromDSP ... ; and again, repeatedly
//
// Every one of those waits needs a reply that only a running DSP core can produce. The audio
// thread is created at a HIGHER priority than its creator, so it preempts immediately and
// then never yields — which parks the whole boot behind a handshake that cannot complete.
//
// Audio is host-side in this port and the JAS mixer is not ported, so the game is silent by
// omission (the same state as the decomp runtime). Skipping the microcode boot keeps that
// true and unblocks everything downstream. When the audio arc lands this must be revisited:
// the replacement is a host mixer, never a DSP core.
void dsp_boot_task(CPUState& cpu) {
    (void)cpu;
}

// The JASystem DSP task-queue readiness predicate @0x80337ca0:
//
//   lbz r0,-23224(r13) ; cmplwi r0,1 ; return (r0 == 1)
//
// That byte is set by the DSP task-completion interrupt. No DSP runs here, so it stays 0 and
// every task submission spins forever waiting for a slot (observed: DSPInterface::initBuffer,
// reached from Driver::init on the audio thread, which preempts its creator and never yields).
//
// Reporting "ready" is the truthful answer for a queue nothing is ever queued into: with no
// DSP, a submitted task neither occupies a slot nor needs draining. This deliberately leaves
// the surrounding allocation work running — initBuffer still allocates its FX buffers, which
// later JAIData::initData expects to exist.
void dsp_task_ready(CPUState& cpu) {
    cpu.gpr[3] = 1;
}

} // namespace

SB_OVERRIDE(0x80337ca0u, dsp_task_ready, "JASystem DSP task-queue ready",
            "no DSP consumes tasks, so a slot is always free")
SB_OVERRIDE(0x8035406cu, dsp_boot_task, "__DSP_boot_task",
            "no DSP core exists to answer the microcode-boot mailbox handshake")
SB_OVERRIDE(0x803433b4u, os_init_audio_system, "__OSInitAudioSystem",
            "no DSP in a host-audio port; retail spins on the DSPCR reset handshake")
