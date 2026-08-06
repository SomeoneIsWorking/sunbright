// dev_si.cpp — SI (serial interface): the four controller ports.
//
// SI polls the controllers and DMAs their status into a buffer. This port takes input from
// the host instead, so what SI needs to provide here is the TRANSPORT and a coherent
// "nothing is transferring right now" status — enough for SIInit and the polling setup to
// run. Actual button state arrives through a PAD-level override, not by assembling SI
// response packets, because the host hands us decoded input rather than a wire protocol.
//
// Until that override exists, no controller is reported as connected. That is a truthful
// answer (there really is no pad wired up yet), not a fabricated one — unlike inventing
// button data, which would be indistinguishable from a real pad reading all-zero.

#include "mmio.h"

#include <lucent/log.h>

#include <cstdlib>
#include <cstring>

namespace {

constexpr u32 SI_BASE = 0xCC006400;
constexpr u32 SI_SIZE = 0x100;

constexpr u32 SI_POLL   = SI_BASE + 0x30;
constexpr u32 SI_COMCSR = SI_BASE + 0x34;   // communication control/status
constexpr u32 SI_STATUS = SI_BASE + 0x38;

// SI_COMCSR bit 0 starts a transfer; it reads back clear once the transfer is done. With no
// bus latency there is nothing to observe in flight.
constexpr u32 COMCSR_TSTART = 0x1u;

u32 g_reg[SI_SIZE / 4];

u32& reg(u32 addr) {
    const u32 i = (addr - SI_BASE) >> 2;
    if (i >= SI_SIZE / 4) {
        lucent::error("si", "register index out of range for address 0x{:08x}", addr);
        std::abort();
    }
    return g_reg[i];
}

u32 si_read(u32 ea, unsigned width) {
    if (width != 4) {
        lucent::error("si", "unsupported {}-byte read @ 0x{:08x}", width, ea);
        std::abort();
    }
    return reg(ea & ~3u);
}

void si_write(u32 ea, unsigned width, u32 value) {
    if (width != 4) {
        lucent::error("si", "unsupported {}-byte write @ 0x{:08x}", width, ea);
        std::abort();
    }
    const u32 a = ea & ~3u;
    reg(a) = value;
    if (a == SI_COMCSR) reg(a) &= ~COMCSR_TSTART;   // completes immediately
}

} // namespace

void si_device_init() {
    std::memset(g_reg, 0, sizeof(g_reg));
    reg(SI_POLL)   = 0;
    reg(SI_COMCSR) = 0;
    reg(SI_STATUS) = 0;   // no channel reports a connected device yet
    mmio_register(MmioDevice{SI_BASE, SI_BASE + SI_SIZE, "si", &si_read, &si_write});
}
