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

// Commands this chip understands, as a full 32-bit command word. Only the one the game
// actually issues is implemented; anything else aborts naming the command, rather than
// being quietly treated as a SRAM access.
constexpr u32 CMD_SRAM_READ = 0x20000100;

u8  g_sram[kSramSize];
u32 g_command = 0;

void imm_write(u32 value, u32 len) {
    if (len != 4) {
        lucent::error("sram", "immediate write of {} bytes (0x{:08x}) — only the 4-byte "
                              "command word is understood", len, value);
        std::abort();
    }
    g_command = value;
    if (g_command != CMD_SRAM_READ) {
        lucent::error("sram", "unimplemented EXI command 0x{:08x} (RTC is 0x20000000, the "
                              "IPL font ROM is a range). Implement it rather than letting it "
                              "read as SRAM.", g_command);
        std::abort();
    }
}

u32 imm_read(u32 len) {
    lucent::error("sram", "immediate read of {} bytes is not implemented (command 0x{:08x})",
                  len, g_command);
    std::abort();
}

void dma(u32 guest_addr, u32 len, bool to_device) {
    if (g_command != CMD_SRAM_READ) {
        lucent::error("sram", "DMA with no command selected (0x{:08x})", g_command);
        std::abort();
    }
    if (to_device) {
        lucent::error("sram", "writing SRAM back is not implemented yet — settings changed "
                              "in-game would silently fail to persist");
        std::abort();
    }
    if (len > kSramSize) {
        lucent::error("sram", "read of {} bytes exceeds the {}-byte chip", len, kSramSize);
        std::abort();
    }
    const u32 off = guest_addr & 0x01FFFFFFu;
    if (off + len > 0x01800000u) {
        lucent::error("sram", "DMA target 0x{:08x} is outside MEM1", guest_addr);
        std::abort();
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

    exi_attach(ExiDevice{0, 1, "sram/rtc", &imm_write, &imm_read, &dma});
}
