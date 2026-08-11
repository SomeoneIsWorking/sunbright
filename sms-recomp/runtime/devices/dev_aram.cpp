// dev_aram.cpp — ARAM (the GameCube's 16 MB auxiliary audio RAM) and its DMA engine.
//
// WHY A DEVICE AND NOT AN SDK OVERRIDE. The alternative was overriding ARInit/ARStartDMA
// with a host buffer and memcpy. That would have meant reverse-engineering every SDA
// global ARInit leaves behind (ARGetSize/ARGetBaseAddress and the whole ARQ layer read
// them) and reproducing them by hand — a large surface to get subtly wrong. ARAM is, by
// contrast, one of the simplest devices on the machine: a flat buffer, four address
// registers and a byte count. Modelling it lets the game's OWN ARInit and sizing code run
// for real, so those globals are computed rather than fabricated.
//
// The sizing routine at 0x80353090 is the reason this works: it writes 0xDEADBEEF /
// 0xBAD0BAD1 patterns into MEM1, DMAs them out to ARAM, DMAs them back, and compares, to
// discover the ARAM size from aliasing behaviour. Against a real 16 MB buffer that
// probe returns the truth with no help from us.
//
// DMA IS SYNCHRONOUS. The copy completes inside the register write that starts it, so the
// busy bit is never observed set. This is the same "synchronous unthrottled I/O" the
// decomp runtime uses (ARAM copies run at the enqueue site), and it sidesteps the fact
// that the standalone runtime has no interrupt delivery yet. It is NOT a shortcut that
// has to be undone: hiding latency the host does not have would mean inventing a delay
// and then building machinery to wait for it.

#include "mmio.h"
#include "aram.h"
#include "../intrinsics.h"

#include <lucent/log.h>

#include <cstdlib>
#include <cstring>
#include <sys/mman.h>

extern u8* g_ram_base;

// AID (0xCC005030..0xCC00503C) is a separate device that used to be swallowed by this one.
// Its init is chained from here, not from rt_core.cpp, so that a STRONG reference pulls
// dev_aid.o out of the static archive — its other export (sbr_audio_frame) is referenced only
// weakly, which would not extract it, and the device would silently never register.
void aid_device_init();

namespace {

// The retail machine has 16 MB of ARAM. The game asks the hardware rather than assuming,
// so this size is what its probe will discover.
constexpr u32 kAramSize = 0x01000000;

// Register offsets within the DSP/ARAM block. Named from what the code does with them;
// the DMA quartet is confirmed by the read-modify-write sequence at 0x8035316c..0x803531ec.
constexpr u32 AR_INFO       = 0x5012;   // ARAM configuration
constexpr u32 AR_MODE       = 0x5016;   // bit 0 polled by ARInit before anything else
constexpr u32 AR_REFRESH    = 0x501A;
constexpr u32 AR_DMA_MM_H   = 0x5020;   // main-memory address, high half
constexpr u32 AR_DMA_MM_L   = 0x5022;
constexpr u32 AR_DMA_AR_H   = 0x5024;   // ARAM address, high half
constexpr u32 AR_DMA_AR_L   = 0x5026;
constexpr u32 AR_DMA_CNT_H  = 0x5028;   // byte count; bit 31 selects direction
constexpr u32 AR_DMA_CNT_L  = 0x502A;   // writing the LOW half starts the transfer

u8* g_aram = nullptr;

// Every register in the block, as halfwords, so an unmodelled one still reads back what
// was written instead of returning a fabricated value.
u16 g_reg[0x40];

u16& reg(u32 off) { return g_reg[(off - 0x5000) >> 1]; }

u32 reg32(u32 off_hi) { return ((u32)reg(off_hi) << 16) | reg(off_hi + 2); }

// Shared by the register-level DMA and the ARQ seam above it.
void do_dma(u32 mm, u32 ar, u32 len, bool to_mram) {
    const u32 mm_off = mm & 0x01FFFFFFu;
    const u32 ar_off = ar & (kAramSize - 1);   // ARAM wraps; the size probe relies on it

    if (mm_off + len > 0x01800000u || ar_off + len > kAramSize) {
        lucent::error("aram", "DMA out of range: mm=0x{:08x} ar=0x{:08x} len=0x{:x} {}",
                      mm, ar, len, to_mram ? "AR->MM" : "MM->AR");
        std::abort();
    }
    if (to_mram) sb_watch_range(mm_off, len, "ARAM DMA (dev_aram, AR->MM)");
    if (to_mram) std::memcpy(g_ram_base + mm_off, g_aram + ar_off, len);
    else         std::memcpy(g_aram + ar_off, g_ram_base + mm_off, len);

    lucent::debug("aram", "DMA {} mm=0x{:08x} ar=0x{:08x} len=0x{:x}",
                  to_mram ? "AR->MM" : "MM->AR", mm, ar, len);
}

void run_dma() {
    const u32 mm    = reg32(AR_DMA_MM_H);
    const u32 ar    = reg32(AR_DMA_AR_H);
    const u32 cnt32 = reg32(AR_DMA_CNT_H);

    // Bit 31 of the count register selects direction: set = ARAM -> main memory.
    const bool to_mram = (cnt32 & 0x80000000u) != 0;
    const u32  len     = cnt32 & 0x7FFFFFFFu;
    if (len == 0) return;

    do_dma(mm, ar, len, to_mram);
}

u32 ar_read(u32 ea, unsigned width) {
    const u32 o = ea & 0xFFFFu;   // 0x50xx

    if (width == 2) {
        u16 v = reg(o & ~1u);
        // ARAM in this port is always present and ready; there is no reset handshake to
        // complete. ARInit spins on this bit before touching anything else.
        if ((o & ~1u) == AR_MODE) v |= 1;
        return v;
    }
    if (width == 4) return ((u32)reg(o) << 16) | reg(o + 2);
    // Byte access to these registers is not something the SDK does; if it starts, the
    // right response is to find out why rather than invent a value.
    lucent::error("aram", "unsupported {}-byte read @ 0x{:08x}", width, ea);
    std::abort();
}

void ar_write(u32 ea, unsigned width, u32 value) {
    const u32 o = ea & 0xFFFFu;

    if (width == 2) {
        reg(o & ~1u) = (u16)value;
        if ((o & ~1u) == AR_DMA_CNT_L) run_dma();
        return;
    }
    if (width == 4) {
        reg(o)     = (u16)(value >> 16);
        reg(o + 2) = (u16)value;
        if (o == AR_DMA_CNT_H) run_dma();
        return;
    }
    lucent::error("aram", "unsupported {}-byte write @ 0x{:08x}", width, ea);
    std::abort();
}

} // namespace

void aram_dma(u32 mram_addr, u32 aram_addr, u32 len, bool to_mram) {
    do_dma(mram_addr, aram_addr, len, to_mram);
}

// Explicit init rather than a static initializer: this file exports nothing else, so in a
// static archive the linker would discard the whole object and the device would silently
// never register (exactly how the first override failed to install).
// The ARAM backing store, for the DSP voice mixer. On hardware the DSP reads sample data
// straight out of ARAM; here the mixer is host code and needs the same window. Exposed as an
// accessor rather than a global so a caller cannot use it before aram_device_init() has
// allocated it — it returns null until then, and the mixer treats null as "no ARAM yet".
extern "C" const u8* sbr_aram_base() { return g_aram; }
extern "C" u32 sbr_aram_size() { return g_aram ? kAramSize : 0u; }

void aram_device_init() {
    {
        void* p = mmap(nullptr, kAramSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            lucent::error("aram", "failed to map {} MB of ARAM", kAramSize >> 20);
            std::abort();
        }
        g_aram = (u8*)p;
        std::memset(g_reg, 0, sizeof(g_reg));
        // AR_INFO / AR_REFRESH power-on values the SDK reads before writing its own.
        reg(AR_INFO)    = 0;
        reg(AR_REFRESH) = 0;
        // The ARAM registers only. The DSP mailboxes and CSR (0x5000-0x500B) below, and the
        // AID audio-DMA engine (0x5030-0x503C) above, share this address space but are
        // different devices; claiming them would silently swallow accesses that belong
        // elsewhere. AID in particular spent this port's whole life being absorbed here as
        // inert halfwords, which is why nothing ever drove the audio heartbeat.
        mmio_register(MmioDevice{0xCC005012u, 0xCC005030u, "aram", &ar_read, &ar_write});
        aid_device_init();
        // 0x503C..0x5040 is the tail of the block; nothing is known to live there, so leave it
        // unclaimed and LOUD rather than answering for registers we cannot name.
        lucent::info("aram", "{} MB ARAM ready @ {}", kAramSize >> 20, (void*)g_aram);
    }
}
