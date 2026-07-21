// dev_di.cpp — DI (disc interface) registers.
//
// DI is how the console talks to the optical drive: a three-word command register set, a
// DMA target, and status/cover registers. This models the register block; actual disc
// COMMANDS are not served yet and abort naming themselves, so the command set gets
// implemented from evidence rather than from a guess about what the game uses.
//
// The eventual shape is the decomp runtime's: every read completes inline and fires its
// callback before returning (extern/aurora/lib/dolphin/dvd). Whether the recomp serves file
// I/O down here at the register level or higher up via DVD-library overrides is still open
// — CLAUDE.md names DVD as an override seam, which argues for the latter, but the OS also
// pokes these registers directly during init, which is what this file is for.

#include "mmio.h"
#include "disc.h"

#include <lucent/log.h>

#include <cstdlib>
#include <cstring>
#include <vector>

extern void rt_dump_guest_stack(const char* why);
extern u8* g_ram_base;

namespace {

constexpr u32 DI_BASE = 0xCC006000;
constexpr u32 DI_SIZE = 0x40;

constexpr u32 DI_SR      = DI_BASE + 0x00;   // status / interrupt flags
constexpr u32 DI_CVR     = DI_BASE + 0x04;   // cover state
constexpr u32 DI_CMDBUF0 = DI_BASE + 0x08;
constexpr u32 DI_CMDBUF1 = DI_BASE + 0x0C;
constexpr u32 DI_CMDBUF2 = DI_BASE + 0x10;
constexpr u32 DI_MAR     = DI_BASE + 0x14;   // DMA destination in main memory
constexpr u32 DI_LENGTH  = DI_BASE + 0x18;
constexpr u32 DI_CR      = DI_BASE + 0x1C;   // control: TSTART starts the command
constexpr u32 DI_CFG     = DI_BASE + 0x24;   // drive/console revision, low byte

constexpr u32 CR_TSTART = 0x1u;

// Drive commands, in the top byte of CMDBUF0.
constexpr u32 CMD_INQUIRY   = 0x12;   // 32 bytes of drive identification
constexpr u32 CMD_READ      = 0xA8;   // CMDBUF1 = offset >> 2, CMDBUF2 = byte count
constexpr u32 CMD_SEEK      = 0xAB;
constexpr u32 CMD_STOPMOTOR = 0xE3;

// DI_CVR bit 0 reports the lid open. A disc is present, so it stays clear — reporting an
// open lid would send the game into its "please insert disc" path.
constexpr u32 CVR_COVER_OPEN = 0x1u;

u32 g_reg[DI_SIZE / 4];

u32& reg(u32 addr) {
    const u32 i = (addr - DI_BASE) >> 2;
    if (i >= DI_SIZE / 4) {
        lucent::error("di", "register index out of range for address 0x{:08x}", addr);
        std::abort();
    }
    return g_reg[i];
}

// Copy into guest main memory the same way the DMA engine would.
void dma_out(const void* src, u32 len) {
    const u32 off = reg(DI_MAR) & 0x01FFFFFFu;
    if (off + len > 0x01800000u) {
        lucent::error("di", "DMA target 0x{:08x} +0x{:x} is outside MEM1", reg(DI_MAR), len);
        std::abort();
    }
    std::memcpy(g_ram_base + off, src, len);
}

void run_command() {
    const u32 cmd = reg(DI_CMDBUF0) >> 24;

    switch (cmd) {
    case CMD_READ: {
        // CMDBUF1 holds the disc offset in 4-byte units; CMDBUF2 the byte count.
        const u64 offset = (u64)reg(DI_CMDBUF1) << 2;
        const u32 len    = reg(DI_CMDBUF2);
        if (len > reg(DI_LENGTH)) {
            lucent::error("di", "read of 0x{:x} bytes exceeds the DMA length 0x{:x}", len,
                          reg(DI_LENGTH));
            std::abort();
        }
        std::vector<u8> buf(len);
        disc_read(offset, buf.data(), len);
        dma_out(buf.data(), len);
        lucent::debug("di", "read 0x{:x} bytes @ disc 0x{:x} -> 0x{:08x}", len, offset,
                      reg(DI_MAR));
        return;
    }
    case CMD_SEEK:
        return;                      // reads carry their own offset; nothing to do
    case CMD_STOPMOTOR:
        lucent::debug("di", "stop motor");
        return;
    case CMD_INQUIRY: {
        // 32 bytes of drive identification (revision, device code, release date). The
        // values are NOT modelled: this port has no drive, and the retail bytes are not
        // something it can verify. Zeros are reported ONCE, loudly, so that if the DVD
        // library turns out to branch on them it is obvious where to look — rather than
        // fabricating a plausible-looking drive that quietly selects a firmware-workaround
        // path. So far nothing observable depends on it.
        static bool warned = false;
        if (!warned) {
            warned = true;
            lucent::warn("di", "drive inquiry answered with zeros — the 32-byte drive ID is "
                               "not modelled and the retail bytes are unverified. If disc "
                               "behaviour later looks firmware-dependent, start here.");
        }
        u8 id[32] = {};
        dma_out(id, sizeof(id) < reg(DI_LENGTH) ? (u32)sizeof(id) : reg(DI_LENGTH));
        return;
    }
    default:
        lucent::error("di", "unimplemented drive command 0x{:02x} (CMDBUF0=0x{:08x})", cmd,
                      reg(DI_CMDBUF0));
        rt_dump_guest_stack("DI command");
        std::abort();
    }
}

u32 di_read(u32 ea, unsigned width) {
    if (width != 4) {
        lucent::error("di", "unsupported {}-byte read @ 0x{:08x}", width, ea);
        std::abort();
    }
    return reg(ea & ~3u);
}

void di_write(u32 ea, unsigned width, u32 value) {
    if (width != 4) {
        lucent::error("di", "unsupported {}-byte write @ 0x{:08x}", width, ea);
        std::abort();
    }
    const u32 a = ea & ~3u;
    reg(a) = value;

    if (a == DI_CR && (value & CR_TSTART)) {
        reg(DI_CR) &= ~CR_TSTART;   // synchronous: never observed in flight
        run_command();
    }
}

} // namespace

void di_device_init() {
    std::memset(g_reg, 0, sizeof(g_reg));
    reg(DI_SR)  = 0;
    reg(DI_CVR) = 0;          // lid closed, disc present
    reg(DI_CFG) = 0;          // retail drive revision; __OSGetDIConfig returns CFG & 0xFF
    (void)CVR_COVER_OPEN;
    mmio_register(MmioDevice{DI_BASE, DI_BASE + DI_SIZE, "di", &di_read, &di_write});
}
