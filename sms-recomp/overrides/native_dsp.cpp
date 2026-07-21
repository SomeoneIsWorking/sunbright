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

} // namespace

SB_OVERRIDE(0x803433b4u, os_init_audio_system, "__OSInitAudioSystem",
            "no DSP in a host-audio port; retail spins on the DSPCR reset handshake")
