// dev_dsp.cpp — DSP interface: mailboxes and the control/status register.
//
// The GameCube DSP is a separate 16-bit core running its own microcode. This port has no
// DSP and will not get one: audio is host-side. So the honest model is a DSP that is
// present but permanently HALTED and idle — control bits read back as written, and every
// interrupt-status bit stays clear because nothing ever raises one.
//
// This is deliberately NOT an attempt to emulate the DSP. It exists so that OS code which
// merely reads the interface during init gets a coherent answer instead of a fabricated
// zero. Any code that actually depends on the DSP running (the reset handshake in
// __OSInitAudioSystem) is handled by an override, because no register model can satisfy a
// handshake whose other end does not exist.

#include "mmio.h"

#include <lucent/log.h>

#include <cstdlib>
#include <cstring>

namespace {

constexpr u32 DSP_MAIL_TO_DSP_H   = 0x5000;
constexpr u32 DSP_MAIL_TO_DSP_L   = 0x5002;
constexpr u32 DSP_MAIL_FROM_DSP_H = 0x5004;
constexpr u32 DSP_MAIL_FROM_DSP_L = 0x5006;
constexpr u32 DSP_CSR             = 0x500A;

// CSR bits that are interrupt STATUS (write-1-to-clear on hardware). With no DSP, no ARAM
// interrupt and no AI interrupt source, none of these can ever become set on their own.
constexpr u16 CSR_AIINT  = 0x0008;
constexpr u16 CSR_ARINT  = 0x0020;
constexpr u16 CSR_DSPINT = 0x0080;
constexpr u16 CSR_STATUS_MASK = CSR_AIINT | CSR_ARINT | CSR_DSPINT;

u16 g_reg[0x10];

u16& reg(u32 off) { return g_reg[(off - 0x5000) >> 1]; }

u32 dsp_read(u32 ea, unsigned width) {
    const u32 o = ea & 0xFFFFu;

    if (width == 2) {
        u16 v = reg(o & ~1u);
        if ((o & ~1u) == DSP_CSR) {
            // Never report a pending interrupt: there is no source for one, and claiming
            // otherwise would send OS code into a handler for an event that did not happen.
            v &= (u16)~CSR_STATUS_MASK;
        }
        return v;
    }
    if (width == 4) return ((u32)reg(o) << 16) | reg(o + 2);
    lucent::error("dsp", "unsupported {}-byte read @ 0x{:08x}", width, ea);
    std::abort();
}

void dsp_write(u32 ea, unsigned width, u32 value) {
    const u32 o = ea & 0xFFFFu;

    if (width == 2) {
        u16 v = (u16)value;
        if ((o & ~1u) == DSP_CSR) {
            // Status bits are write-1-to-clear; they are already clear, so just drop them
            // and keep the control bits the caller set.
            v &= (u16)~CSR_STATUS_MASK;
        }
        reg(o & ~1u) = v;
        return;
    }
    if (width == 4) {
        reg(o)     = (u16)(value >> 16);
        reg(o + 2) = (u16)value;
        return;
    }
    lucent::error("dsp", "unsupported {}-byte write @ 0x{:08x}", width, ea);
    std::abort();
}

} // namespace

// Explicit init, not a static initializer — a static-archive object exporting nothing else
// gets discarded by the linker and the device silently never registers.
void dsp_device_init() {
    std::memset(g_reg, 0, sizeof(g_reg));
    // The mailbox "from DSP" high halfword carries a status bit the SDK polls for ownership;
    // leaving the whole mailbox zero means "no message pending", which is the truth here.
    reg(DSP_MAIL_FROM_DSP_H) = 0;
    reg(DSP_MAIL_FROM_DSP_L) = 0;
    reg(DSP_MAIL_TO_DSP_H)   = 0;
    reg(DSP_MAIL_TO_DSP_L)   = 0;

    // Up to but not including the ARAM registers, which are a separate device.
    mmio_register(MmioDevice{0xCC005000u, 0xCC005012u, "dsp", &dsp_read, &dsp_write});
}
