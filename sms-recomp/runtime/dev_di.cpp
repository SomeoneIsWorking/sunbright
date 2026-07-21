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

#include <lucent/log.h>

#include <cstdlib>
#include <cstring>

extern void rt_dump_guest_stack(const char* why);

namespace {

constexpr u32 DI_BASE = 0xCC006000;
constexpr u32 DI_SIZE = 0x40;

constexpr u32 DI_SR      = DI_BASE + 0x00;   // status / interrupt flags
constexpr u32 DI_CVR     = DI_BASE + 0x04;   // cover state
constexpr u32 DI_CMDBUF0 = DI_BASE + 0x08;
constexpr u32 DI_CR      = DI_BASE + 0x1C;   // control: TSTART starts the command
constexpr u32 DI_CFG     = DI_BASE + 0x24;   // drive/console revision, low byte

constexpr u32 CR_TSTART = 0x1u;

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
        reg(DI_CR) &= ~CR_TSTART;
        lucent::error("di", "disc command 0x{:08x} is not implemented — no disc is mounted. "
                            "Serving zeros would hand the game an empty filesystem and it "
                            "would fail somewhere far from here.",
                      reg(DI_CMDBUF0));
        rt_dump_guest_stack("DI command");
        std::abort();
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
