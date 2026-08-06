// dev_sram.cpp — the SRAM/RTC device on EXI channel 0, chip-select 1.
//
// On a real console this chip holds 64 bytes of battery-backed console settings (language,
// sound mode, screen offset, calibration) plus the real-time clock. The boot ROM validates
// and repairs it; the GAME only reads it.
//
// That last point was checked, not assumed. The only three places in this DOL that touch
// the SRAM mirror at 0x80402640 are:
//   0x80347608  read  — EXISelect(0,1,3), EXIImm(cmd=0x20000100, 4, write), EXIDma(64, read)
//   0x8034773c  lock
//   0x80347490  flush/write-back
// None of them computes or verifies a checksum. So the checksum words are left zero rather
// than fabricating a value from a remembered algorithm: a wrong checksum would be worse
// than none, and an unverifiable "correct" one is not something to claim. If anything ever
// does validate, it will show up as behaviour rather than being silently papered over.

#include "exi.h"
#include "mmio.h"

#include <lucent/log.h>

#include <cstdlib>
#include <cstring>

extern u8* g_ram_base;

namespace {

constexpr u32 kSramSize = 64;

// Field offsets within the 64-byte block.
constexpr u32 S_CHECKSUM     = 0x00;   // u16, and its complement at 0x02
constexpr u32 S_EAD0         = 0x04;
constexpr u32 S_COUNTER_BIAS = 0x0C;
constexpr u32 S_DISPLAY_OFFS = 0x10;   // s8
constexpr u32 S_NTD          = 0x11;
constexpr u32 S_LANGUAGE     = 0x12;
constexpr u32 S_FLAGS        = 0x13;
// SramEx: the per-slot memory-card flash IDs and their checksum bytes.
constexpr u32 S_FLASHID          = 0x14;   // u8[2][12], indexed by EXI channel
constexpr u32 S_FLASHID_LEN      = 12;
constexpr u32 S_FLASHID_CHECKSUM = 0x3A;   // u8[2]

// Commands this chip understands, as a full 32-bit command word. Only the one the game
// actually issues is implemented; anything else aborts naming the command, rather than
// being quietly treated as a SRAM access.
constexpr u32 CMD_SRAM_READ = 0x20000100;

// Anything whose decoded address lands below the SRAM/RTC window is an IPL boot-ROM read.
// The GameCube's boot ROM contains the system fonts (ANSI and Shift-JIS), which games load
// through this same chip. Commands carry the ROM offset in bits 6..30.
constexpr u32 CMD_ADDR_SHIFT = 6;
constexpr u32 IPL_ROM_LIMIT  = 0x00800000;   // SRAM/RTC commands decode above this

u8  g_sram[kSramSize];
u32 g_command = 0;

// Byte offset a write command selected, or kNoWrite when the pending command is a read.
constexpr u32 kNoWrite = 0xFFFFFFFFu;
u32 g_write_offset = kNoWrite;

// Bit 31 of the command word selects a write.
constexpr u32 CMD_WRITE = 0x80000000u;

void imm_write(u32 value, u32 len) {
    if (len != 4) {
        lucent::error("sram", "immediate write of {} bytes (0x{:08x}) — only the 4-byte "
                              "command word is understood", len, value);
        std::abort();
    }
    // A write command is followed by the DATA words themselves, not by further commands. The
    // SDK flushes SRAM with immediate writes (four bytes at a time), so anything arriving while
    // a write is selected is payload — decoding it as a command aborted on "SUNB", the first
    // four bytes of the flash ID being written back.
    if (g_write_offset != kNoWrite) {
        // The chip is 64 bytes and its address pointer simply runs out; the transfer that
        // follows is a new command, not more payload. We cannot observe the chip-select drop
        // from here (the EXI device interface has no deselect hook), so the end of the address
        // space is what ends the write phase.
        if (g_write_offset >= kSramSize) {
            g_write_offset = kNoWrite;
            // fall through and treat this word as a command
        } else {
        // The DATA register presents the word MSB-first, exactly as the chip receives it.
        const u32 n = (kSramSize - g_write_offset) < 4 ? (kSramSize - g_write_offset) : 4;
        for (u32 i = 0; i < n; ++i) g_sram[g_write_offset + i] = (u8)(value >> (8 * (3 - i)));
        g_write_offset += n;
        return;
        }
    }

    g_command = value;
    if (g_command == CMD_SRAM_READ) { g_write_offset = kNoWrite; return; }

    // Write commands set bit 31; the address field is a byte address within the chip, so the
    // SRAM offset is that address minus SRAM's own base. __OSUnlockSramEx(flush) writes back
    // only the region it marked dirty, so this is how settings the guest changes reach the
    // chip — the flash-ID record a card mount updates being the case that surfaced it.
    if (value & CMD_WRITE) {
        const u32 addr = (value >> CMD_ADDR_SHIFT) & 0x1FFFFFFu;
        const u32 sram_base = (CMD_SRAM_READ >> CMD_ADDR_SHIFT) & 0x1FFFFFFu;
        if (addr < sram_base || addr - sram_base >= kSramSize) {
            lucent::error("sram", "write command 0x{:08x} targets chip address 0x{:x}, outside "
                                  "the {}-byte SRAM at 0x{:x}", value, addr, kSramSize,
                          sram_base);
            std::abort();
        }
        g_write_offset = addr - sram_base;
        return;
    }

    const u32 addr = (g_command >> CMD_ADDR_SHIFT) & 0x1FFFFFFu;
    if (addr < IPL_ROM_LIMIT) {
        // IPL boot-ROM read (the system font lives at ~0x1FCF00). We do not have an IPL
        // image and will not ship one — it is copyrighted console firmware. Serving zeros
        // means IPL-font glyphs render blank; the game's own UI fonts come off the disc and
        // are unaffected. Announced once, precisely, so a blank-text symptom is traceable
        // here instead of looking like a font-rendering bug.
        static bool warned = false;
        if (!warned) {
            warned = true;
            lucent::warn("sram", "IPL boot-ROM read at 0x{:06x} — no IPL image is available "
                                 "(copyrighted console firmware), serving zeros. Text drawn "
                                 "with the IPL system font will be blank. Disc fonts are "
                                 "unaffected.", addr);
        }
        return;
    }

    lucent::error("sram", "unimplemented EXI command 0x{:08x} (SRAM is 0x20000100, RTC is "
                          "0x20000000). Implement it rather than letting it read as SRAM.",
                  g_command);
    std::abort();
}

u32 imm_read(u32 len) {
    lucent::error("sram", "immediate read of {} bytes is not implemented (command 0x{:08x})",
                  len, g_command);
    std::abort();
}

void dma(u32 guest_addr, u32 len, bool to_device) {
    const u32 addr = (g_command >> CMD_ADDR_SHIFT) & 0x1FFFFFFu;
    const bool ipl = addr < IPL_ROM_LIMIT;

    if (to_device) {
        // Write-back of the region a write command selected. The chip is battery-backed on a
        // console; here it lives only for the run, which is the honest equivalent of a console
        // whose settings are re-derived at boot — what matters is that the guest reads back
        // what it wrote, so its own checksums stay consistent.
        if (g_write_offset == kNoWrite) {
            lucent::error("sram", "SRAM DMA write with no write command selected (0x{:08x})",
                          g_command);
            std::abort();
        }
        if (g_write_offset + len > kSramSize) {
            lucent::error("sram", "write of {} bytes at offset 0x{:x} runs past the {}-byte chip",
                          len, g_write_offset, kSramSize);
            std::abort();
        }
        const u32 src = guest_addr & 0x01FFFFFFu;
        std::memcpy(g_sram + g_write_offset, g_ram_base + src, len);
        lucent::debug("sram", "wrote {} bytes at offset 0x{:x}", len, g_write_offset);
        return;
    }

    if (!ipl && g_command != CMD_SRAM_READ) {
        lucent::error("sram", "DMA with no command selected (0x{:08x})", g_command);
        std::abort();
    }
    if (!ipl && len > kSramSize) {
        lucent::error("sram", "read of {} bytes exceeds the {}-byte chip", len, kSramSize);
        std::abort();
    }
    const u32 off = guest_addr & 0x01FFFFFFu;
    if (off + len > 0x01800000u) {
        lucent::error("sram", "DMA target 0x{:08x} is outside MEM1", guest_addr);
        std::abort();
    }
    if (ipl) {
        std::memset(g_ram_base + off, 0, len);   // no IPL image; see imm_write
        lucent::debug("sram", "IPL ROM read of {} bytes -> 0x{:08x} (zeros)", len, guest_addr);
        return;
    }
    std::memcpy(g_ram_base + off, g_sram, len);
    lucent::debug("sram", "read {} bytes -> 0x{:08x}", len, guest_addr);
}

} // namespace

void sram_device_init() {
    std::memset(g_sram, 0, sizeof(g_sram));

    // Defaults for a US console. Language 0 is English, which is what this disc expects;
    // zero display offset and no flags is the neutral factory state.
    g_sram[S_LANGUAGE]     = 0;
    g_sram[S_FLAGS]        = 0;
    g_sram[S_NTD]          = 0;
    g_sram[S_DISPLAY_OFFS] = 0;
    (void)S_CHECKSUM; (void)S_EAD0; (void)S_COUNTER_BIAS;

    // Flash-ID records. SRAM remembers the flash ID of the card last seen in each slot plus a
    // checksum byte, so the OS can tell that a different card has been swapped in. The CARD
    // library VERIFIES it during mount (0x803584e4..0x80358504): it sums flashID[chan][0..11]
    // and requires flashIDCheckSum[chan] to equal the one's complement of that sum, returning
    // CARD_RESULT_IOERROR (-5) otherwise — which is exactly what a card mount hit here.
    //
    // Offsets read off the SDK: the accessor at 0x80347798 returns SRAM+0x14 (the flash-ID
    // array, 12 bytes per channel), and the mount indexes the checksum at +0x26 past it.
    //
    // We have no recorded flash IDs, which is a legitimate console state — but the block must
    // still be SELF-CONSISTENT, exactly as an IPL that wrote both fields together would leave
    // it. Computing the checksum from the bytes actually present keeps the invariant true
    // whatever those bytes are, rather than hardcoding the value that happens to satisfy it.
    for (u32 chan = 0; chan < 2; ++chan) {
        u8 sum = 0;
        for (u32 i = 0; i < S_FLASHID_LEN; ++i) sum += g_sram[S_FLASHID + chan * S_FLASHID_LEN + i];
        g_sram[S_FLASHID_CHECKSUM + chan] = (u8)~sum;
    }

    exi_attach(ExiDevice{0, 1, "sram/rtc", &imm_write, &imm_read, &dma});
}
