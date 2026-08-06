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
#include "exi.h"
#include "guest_sched.h"
#include "intrinsics.h"

#include <lucent/log.h>

// Defined in rt_core.cpp — names the guest functions that led here.
extern void rt_dump_guest_stack(const char* why);

#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr u32 EXI_BASE     = 0xCC006800;
constexpr u32 kChannels    = 3;
constexpr u32 kChannelSize = 0x14;    // CSR, MAR, LENGTH, CR, DATA

// Per-channel register indices.
constexpr u32 R_CSR = 0, R_MAR = 1, R_LEN = 2, R_CR = 3, R_DATA = 4;

// EXI_CSR chip-select lines live in bits 7..9, one per device.
constexpr u32 CSR_EXIINT   = 1u << 1;    // EXI interrupt status  (write 1 to clear)
constexpr u32 CSR_TCINT    = 1u << 3;    // transfer-complete status (write 1 to clear)
constexpr u32 CSR_EXTINT   = 1u << 11;   // insertion/removal event  (write 1 to clear)
constexpr u32 CSR_W1C_MASK = CSR_EXIINT | CSR_TCINT | CSR_EXTINT;

constexpr u32 CSR_EXT      = 1u << 12;   // a device is connected in the external slot
constexpr u32 CSR_CS_SHIFT = 7;
constexpr u32 CSR_CS_MASK  = 0x7u << CSR_CS_SHIFT;

// EXI_CR: bit0 starts the transfer and reads back clear once it completes; bit1 selects
// DMA over an immediate transfer; bits 2-3 are the direction; bits 4-5 hold (length - 1)
// for immediate transfers.
constexpr u32 CR_TSTART = 0x1u;
constexpr u32 CR_DMA    = 0x2u;
constexpr u32 CR_RW_SHIFT = 2, CR_RW_MASK = 0x3u << CR_RW_SHIFT;
constexpr u32 CR_LEN_SHIFT = 4, CR_LEN_MASK = 0x3u << CR_LEN_SHIFT;

std::vector<ExiDevice>& attached() {
    static std::vector<ExiDevice> v;
    return v;
}

const ExiDevice* find_device(u32 ch, int dev) {
    for (const auto& d : attached())
        if (d.channel == ch && d.device == (u32)dev) return &d;
    return nullptr;
}

u32 g_reg[kChannels][5];

int selected_device(u32 ch) {
    const u32 cs = (g_reg[ch][R_CSR] & CSR_CS_MASK) >> CSR_CS_SHIFT;
    // Exactly one line should be asserted; the SDK never drives two.
    for (int d = 0; d < 3; d++)
        if (cs == (1u << d)) return d;
    return -1;   // none selected
}

void start_transfer(u32 ch, u32 cr) {
    const int dev = selected_device(ch);
    if (dev < 0) {
        lucent::error("exi", "channel {} started a transfer with no chip-select asserted "
                             "(CSR=0x{:08x})", ch, g_reg[ch][R_CSR]);
        std::abort();
    }
    const ExiDevice* d = find_device(ch, dev);
    if (!d) {
        lucent::error("exi", "channel {} device {} has no implementation — EXI transport is "
                             "modelled but nothing is attached. Returning bus-idle bytes "
                             "would fake a broken console. Implement this device.", ch, dev);
        rt_dump_guest_stack("EXI transfer to an unimplemented device");
        std::abort();
    }

    const bool to_device = ((cr & CR_RW_MASK) >> CR_RW_SHIFT) != 0;

    if (cr & CR_DMA) {
        d->dma(g_reg[ch][R_MAR], g_reg[ch][R_LEN], to_device);
        return;
    }
    const u32 len = ((cr & CR_LEN_MASK) >> CR_LEN_SHIFT) + 1;
    if (to_device) d->imm_write(g_reg[ch][R_DATA], len);
    else           g_reg[ch][R_DATA] = d->imm_read(len);
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
    if (r == R_CSR) {
        // Bit 12 (EXT) reports "a device is connected in this channel's external slot". The
        // SDK's EXIProbe reads it to decide whether a memory card is present, WITHOUT issuing
        // any command — so a device attached here is invisible to the guest until this bit
        // says so. Device 0 is the external slot (memory card); device 1 on channel 0 is the
        // internal SRAM/RTC chip, which is not an insertable device and must not set EXT.
        u32 v = g_reg[ch][R_CSR] & ~CSR_EXT;
        if (find_device(ch, 0) != nullptr) v |= CSR_EXT;
        return v;
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

    if (r == R_CSR) {
        // The interrupt-status bits are WRITE-1-TO-CLEAR: the SDK writes a 1 to acknowledge an
        // event, and hardware then reads back 0. Storing the write verbatim inverts that — the
        // bit reads back SET forever, so the guest sees a permanently pending event.
        //
        // This is not theoretical. EXIProbe (0x8036a2d8) resets its insertion-debounce
        // timestamp every time it observes EXTINT, so a stuck EXTINT meant the card never
        // finished settling: EXIProbeEx returned 0 (still settling) on all 400 traced calls and
        // the game reported the card as damaged. Nothing in this runtime ever RAISES these
        // bits, so after masking they simply stay clear, which is the truth here.
        value &= ~CSR_W1C_MASK;
    }

    g_reg[ch][r] = value;

    if (r == R_CR && (value & CR_TSTART)) {
        g_reg[ch][R_CR] &= ~CR_TSTART;   // completes before the write returns
        start_transfer(ch, value);
    }
}

} // namespace

void exi_attach(const ExiDevice& dev) {
    if (find_device(dev.channel, (int)dev.device)) {
        lucent::error("exi", "channel {} device {} already has something attached",
                      dev.channel, dev.device);
        std::abort();
    }
    attached().push_back(dev);
    lucent::info("exi", "attached {} on channel {} device {}", dev.name, dev.channel,
                 dev.device);
}

void exi_device_init() {
    std::memset(g_reg, 0, sizeof(g_reg));
    mmio_register(MmioDevice{EXI_BASE, EXI_BASE + kChannels * kChannelSize, "exi",
                             &exi_read, &exi_write});
}
