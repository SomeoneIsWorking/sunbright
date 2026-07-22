// dev_card.cpp — the memory card in slot A, on EXI channel 0 chip-select 0.
//
// The guest already HAS a complete, working memory-card driver: the recompiled CARD library
// talks to this slot over EXI. Attaching a card here lets that code run as retail does, with
// every CARDFileInfo/CARDStat staying in guest layout. Overriding the CARD SDK entry points
// onto a host implementation would instead need per-structure marshalling across the guest
// boundary at a much wider API than DVD's — the exact boundary problem that made the old
// hybrid runtime untenable.
//
// The card is backed by a real Dolphin card image so saves persist and are interchangeable
// with Dolphin. The path comes from SBR_CARD_A, else Dolphin's own location; the image is
// mapped read/write and written back in place.
//
// Commands are implemented as the guest exercises them. Anything unrecognised ABORTS naming
// the command rather than returning plausible bytes: a card that answers wrongly corrupts
// saves, which is far worse than a card that is absent (the honest state before this file).

#include "exi.h"
#include "mmio.h"

#include <lucent/log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern u8* g_ram_base;

namespace {

// Slot A is EXI channel 0, chip-select 0. (Channel 0 CS1 is the SRAM/RTC chip — dev_sram.cpp.)
constexpr u32 kChannel = 0;
constexpr u32 kDevice  = 0;

// Card commands, from the guest's own driver usage. Each is the first byte of an immediate
// write; the rest of the sequence differs per command.
constexpr u8 CMD_READ_ID     = 0x00;
constexpr u8 CMD_READ_STATUS = 0x83;
constexpr u8 CMD_CLEAR_STAT  = 0x89;
constexpr u8 CMD_SET_INTR_EN = 0x81;   // 0x81 <0|1>: enable/disable the card's interrupt
constexpr u8 CMD_READ_BLOCK  = 0x52;
constexpr u8 CMD_WRITE_PAGE  = 0xF2;
constexpr u8 CMD_ERASE_SECT  = 0xF1;

// Card status bits the driver polls. READY means "not busy"; UNLOCKED reports that the card
// does not require the (retail-only) unlock handshake.
constexpr u8 STATUS_READY    = 0x01;
constexpr u8 STATUS_UNLOCKED = 0x40;

std::vector<u8> g_image;      // the whole card, mapped in memory
std::string     g_path;
bool            g_present = false;
bool            g_dirty   = false;

// Command state carried from the immediate write that starts a command to the DMA that
// completes it.
u8  g_cmd  = 0;
u32 g_addr = 0;
u8  g_addr_buf[4] = {0, 0, 0, 0};
u32 g_addr_want = 0;      // address bytes still outstanding for the command in flight
bool g_dummy_phase = false;   // clocking out latency bytes between a read address and its DMA

// The driver scatters the byte offset across the four address bytes (0x803557ac..0x803557c8):
//   b0 = (addr >> 29) & 0x03   b1 = (addr >> 21) & 0xFF
//   b2 = (addr >> 19) & 0x03   b3 = (addr >> 12) & 0x7F
// Those fields are contiguous bits 12..30, and the driver masks the address with 0xFFFFF000
// beforehand, so the low 12 bits are always zero. This is the exact inverse.
void decode_address() {
    g_addr = ((u32)(g_addr_buf[0] & 0x03) << 29) | ((u32)g_addr_buf[1] << 21) |
             ((u32)(g_addr_buf[2] & 0x03) << 19) | ((u32)(g_addr_buf[3] & 0x7F) << 12);
    static int n = 0;
    if (n < 12) {
        ++n;
        lucent::debug("card", "read addr bytes {:02x} {:02x} {:02x} {:02x} -> offset 0x{:x}",
                      g_addr_buf[0], g_addr_buf[1], g_addr_buf[2], g_addr_buf[3], g_addr);
    }
}
u8  g_status = STATUS_READY | STATUS_UNLOCKED;
bool g_intr_enabled = false;   // set by command 0x81; nothing here raises an interrupt

// The EXI device ID identifies the card's capacity. The SDK maps it to the retail block
// counts (59/123/251/507/1019/2043 usable blocks), so the value doubles with each size step:
//   512 KB -> 0x04,  1 MB -> 0x08,  2 MB -> 0x10,  4 MB -> 0x20,  8 MB -> 0x40,  16 MB -> 0x80
// Getting this wrong is not silent: the guest reported "The device in Slot A is not supported"
// when the ID did not match any entry in its table.
u32 card_id_for_size(size_t bytes) {
    switch (bytes >> 19) {          // half-MiB units, so 512 KB is 1
    case 1:  return 0x04;           // 512 KB
    case 2:  return 0x08;           //   1 MB
    case 4:  return 0x10;           //   2 MB
    case 8:  return 0x20;           //   4 MB
    case 16: return 0x40;           //   8 MB
    case 32: return 0x80;           //  16 MB
    default: return 0;
    }
}

void flush_image() {
    if (!g_dirty || !g_present) return;
    FILE* f = std::fopen(g_path.c_str(), "r+b");
    if (f == nullptr) {
        lucent::error("card", "cannot reopen {} to write back saves", g_path);
        std::abort();
    }
    std::fwrite(g_image.data(), 1, g_image.size(), f);
    std::fclose(f);
    g_dirty = false;
}

void card_imm_write(u32 value, u32 len) {
    // Immediate transfers are left-justified in the DATA register, so the command byte is the
    // most significant one regardless of length.
    const u8 b0 = (u8)(value >> 24);
    const u8 b1 = (u8)(value >> 16);
    const u8 b2 = (u8)(value >> 8);
    const u8 b3 = (u8)value;

    if (g_dummy_phase) {
        // Between a read command's address and its DMA the driver clocks out "latency" bytes
        // (0x803557f4: EXIImmEx of card->latency bytes from a scratch buffer). The card ignores
        // whatever is written during that window — it is filling the pipeline, not issuing a
        // command. Without this the dummy 0xFF bytes were decoded as a command and aborted.
        return;
    }
    if (g_addr_want > 0) {
        // Address bytes continuing a command already in flight. EXIImmEx splits the driver's
        // 5-byte command into a 4-byte and a 1-byte immediate transfer, so they arrive in
        // pieces and have to be reassembled before decoding.
        for (u32 k = 0; k < len && g_addr_want > 0; ++k) {
            g_addr_buf[4 - g_addr_want] = (u8)(value >> (8 * (3 - k)));
            --g_addr_want;
        }
        if (g_addr_want == 0) decode_address();
        return;
    }

    {   // Every distinct command the driver issues, once each: the sequence it actually
        // performs is the specification, and guessing which commands matter is what produced
        // the earlier wrong answers.
        static bool seen[256] = {};
        if (!seen[b0]) {
            seen[b0] = true;
            lucent::debug("card", "command 0x{:02x} (imm write 0x{:08x}, {} bytes)", b0, value,
                          len);
        }
    }
    g_cmd = b0;
    switch (b0) {
    case CMD_READ_ID:
    case CMD_READ_STATUS:
        return;
    case CMD_CLEAR_STAT:
        g_status |= STATUS_READY;
        return;
    case CMD_SET_INTR_EN:
        // The driver builds this at 0x803546c0 as 0x8101_0000 or 0x8100_0000 and writes two
        // bytes, so the payload is a boolean: enable or disable the card's completion
        // interrupt. Record it and answer nothing. This runtime completes every transfer
        // before the register write returns, so there is no interrupt to gate — but the card
        // must still ACCEPT the command, and a card that silently ignored it would be lying
        // about a control it is expected to honour.
        g_intr_enabled = (b1 != 0);
        return;
    case CMD_READ_BLOCK:
        // 0x52 followed by FOUR address bytes (the driver builds a 5-byte command at
        // 0x8035579c). This first transfer carries as many of them as it has room for.
        g_addr_want = 4;
        for (u32 k = 1; k < len && g_addr_want > 0; ++k) {
            g_addr_buf[4 - g_addr_want] = (u8)(value >> (8 * (3 - k)));
            --g_addr_want;
        }
        if (g_addr_want == 0) { decode_address(); g_dummy_phase = true; }
        (void)b1; (void)b2; (void)b3;
        return;
    case CMD_WRITE_PAGE:
    case CMD_ERASE_SECT:
        g_addr = ((u32)b1 << 16) | ((u32)b2 << 8) | b3;
        return;
    default:
        lucent::error("card", "unimplemented card command 0x{:02x} (imm write 0x{:08x}, {} "
                              "bytes) — answering it wrongly would corrupt saves", b0, value,
                      len);
        std::abort();
    }
}

u32 card_imm_read(u32 len) {
    switch (g_cmd) {
    case CMD_READ_ID: {
        // EXIGetID reads the ID as a full 32-bit word, so it is NOT left-justified the way a
        // short immediate transfer would be.
        const u32 id = card_id_for_size(g_image.size());
        return len >= 4 ? id : (id << (8 * (4 - len)));
    }
    case CMD_READ_STATUS:
        return (u32)g_status << 24;
    default:
        lucent::error("card", "immediate read of {} bytes with no command in flight (last "
                              "command 0x{:02x})", len, g_cmd);
        std::abort();
    }
}

void card_dma(u32 guest_addr, u32 len, bool to_device) {
    if (!g_present) {
        lucent::error("card", "DMA with no card image loaded");
        std::abort();
    }
    if ((size_t)g_addr + len > g_image.size()) {
        lucent::error("card", "card DMA out of range: offset 0x{:x} + {} > {} bytes", g_addr,
                      len, g_image.size());
        std::abort();
    }
    {
        static int n = 0;
        if (n < 12) {
            ++n;
            lucent::debug("card", "DMA {} {} bytes at card offset 0x{:x}",
                          to_device ? "write" : "read", len, g_addr);
        }
    }
    g_dummy_phase = false;   // the DMA is the end of the read command's dummy window
    u8* host = g_ram_base + guest_addr;
    if (to_device) {
        std::memcpy(g_image.data() + g_addr, host, len);
        g_dirty = true;
    } else {
        std::memcpy(host, g_image.data() + g_addr, len);
    }
    g_addr += len;
}

} // namespace

// Explicit init, not a static initializer: an object exporting nothing else is discarded by
// the linker and the device silently never attaches.
void card_device_init() {
    // Dolphin's own card location, so saves are shared with it. Overridable for a scratch card.
    const char* env = std::getenv("SBR_CARD_A");
    if (env != nullptr && env[0] != '\0') {
        g_path = env;
    } else {
        const char* home = std::getenv("HOME");
        if (home == nullptr || home[0] == '\0') return;   // no card; the slot stays empty
        g_path = std::string(home) + "/.local/share/dolphin-emu/GC/MemoryCardA.USA.raw";
    }

    FILE* f = std::fopen(g_path.c_str(), "rb");
    if (f == nullptr) {
        // An absent card is a legitimate console state, and the game handles it. Say so once
        // rather than pretending a card is present.
        lucent::info("card", "no memory card image at {} — slot A is empty", g_path);
        return;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0 || card_id_for_size((size_t)n) == 0) {
        lucent::error("card", "{} is {} bytes, which is not a valid GameCube card size", g_path,
                      n);
        std::abort();
    }
    g_image.resize((size_t)n);
    if (std::fread(g_image.data(), 1, g_image.size(), f) != g_image.size()) {
        lucent::error("card", "short read of {}", g_path);
        std::abort();
    }
    std::fclose(f);
    g_present = true;
    lucent::info("card", "slot A: {} ({} MiB, {} blocks)", g_path, n >> 20, n >> 13);

    exi_attach(ExiDevice{kChannel, kDevice, "card-a", &card_imm_write, &card_imm_read,
                         &card_dma});
    std::atexit(&flush_image);
}
