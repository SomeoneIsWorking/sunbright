// dev_exi.cpp — EXI (external interface): the serial bus carrying SRAM/RTC, memory cards
// and the IPL ROM.
//
// EXI is a TRANSPORT, not a device: each of the three channels can select one of three
// chip-selects, and whatever is attached there speaks its own protocol on top. This file
// models the transport only — the registers, the chip-select lines, and transfer
// completion. Attached devices register separately.
//
// Transfers complete synchronously (the TSTART bit is never observed set), for the same
// reason ARAM DMA does: the host has no bus latency to hide.
//
// Selecting a chip-select with nothing attached is FATAL rather than returning bus-idle
// 0xFF bytes. On real hardware the console always has SRAM/RTC on channel 0, so quietly
// handing back 0xFF would be inventing a broken console — the guest would read a corrupt
// SRAM checksum and silently fall back to defaults, which is exactly the kind of
// plausible-but-wrong behaviour that hides for days.

#include "mmio.h"

#include <lucent/log.h>

#include <cstdlib>
#include <cstring>

namespace {

constexpr u32 EXI_BASE     = 0xCC006800;
constexpr u32 kChannels    = 3;
constexpr u32 kChannelSize = 0x14;    // CSR, MAR, LENGTH, CR, DATA

// Per-channel register indices.
constexpr u32 R_CSR = 0, R_MAR = 1, R_LEN = 2, R_CR = 3, R_DATA = 4;

// EXI_CSR chip-select lines live in bits 7..9, one per device.
constexpr u32 CSR_CS_SHIFT = 7;
constexpr u32 CSR_CS_MASK  = 0x7u << CSR_CS_SHIFT;

// EXI_CR: bit0 starts the transfer and reads back clear once it completes.
constexpr u32 CR_TSTART = 0x1u;

u32 g_reg[kChannels][5];

int selected_device(u32 ch) {
    const u32 cs = (g_reg[ch][R_CSR] & CSR_CS_MASK) >> CSR_CS_SHIFT;
    // Exactly one line should be asserted; the SDK never drives two.
    for (int d = 0; d < 3; d++)
        if (cs == (1u << d)) return d;
    return -1;   // none selected
}

void start_transfer(u32 ch) {
    const int dev = selected_device(ch);
    if (dev < 0) {
        lucent::error("exi", "channel {} started a transfer with no chip-select asserted "
                             "(CSR=0x{:08x})", ch, g_reg[ch][R_CSR]);
        std::abort();
    }
    lucent::error("exi", "channel {} device {} has no implementation — EXI transport is "
                         "modelled but nothing is attached. Returning bus-idle bytes would "
                         "fake a broken console (corrupt SRAM checksum -> silent fallback "
                         "to defaults). Implement this device.",
                  ch, dev);
    std::abort();
}

u32 exi_read(u32 ea, unsigned width) {
    if (width != 4) {
        lucent::error("exi", "unsupported {}-byte read @ 0x{:08x}", width, ea);
        std::abort();
    }
    const u32 off = ea - EXI_BASE;
    const u32 ch  = off / kChannelSize;
    const u32 r   = (off % kChannelSize) / 4;
    if (ch >= kChannels) {
        lucent::error("exi", "read @ 0x{:08x} is outside the three channels", ea);
        std::abort();
    }
    return g_reg[ch][r];
}

void exi_write(u32 ea, unsigned width, u32 value) {
    if (width != 4) {
        lucent::error("exi", "unsupported {}-byte write @ 0x{:08x}", width, ea);
        std::abort();
    }
    const u32 off = ea - EXI_BASE;
    const u32 ch  = off / kChannelSize;
    const u32 r   = (off % kChannelSize) / 4;
    if (ch >= kChannels) {
        lucent::error("exi", "write @ 0x{:08x} is outside the three channels", ea);
        std::abort();
    }

    g_reg[ch][r] = value;

    if (r == R_CR && (value & CR_TSTART)) {
        g_reg[ch][R_CR] &= ~CR_TSTART;   // completes before the write returns
        start_transfer(ch);
    }
}

} // namespace

void exi_device_init() {
    std::memset(g_reg, 0, sizeof(g_reg));
    mmio_register(MmioDevice{EXI_BASE, EXI_BASE + kChannels * kChannelSize, "exi",
                             &exi_read, &exi_write});
}
