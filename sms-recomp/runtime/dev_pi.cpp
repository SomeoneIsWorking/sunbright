// dev_pi.cpp — PI (processor interface): interrupt cause/mask, the GX FIFO pointers, and
// the Flipper revision register.
//
// FLIPPER REVISION. OSInit reads 0xCC00302C, takes its TOP NIBBLE, and folds it into a
// console-revision word:
//
//   lis  r3,0xcc00 ; addi r3,r3,0x3000
//   lwz  r0,0x2c(r3)          ; PI_FLIPPER_REV
//   rlwinm r0,r0,0,0,3        ; keep the top nibble
//   rlwinm r0,r0,4,28,31      ; move it down
//   add  r0,r3,r0             ; add to the existing revision field
//   ...
//   cmplwi r4,0 ; beq -> r4 = 0x10000002      ; explicit "unknown" fallback
//
// We report 0 and let the guest take that fallback, rather than inventing a revision word.
// The real retail value is a specific constant this port cannot verify, and a wrong one
// would silently select a different hardware-workaround path inside the SDK. Taking a
// branch the game itself provides for the unknown case is the honest option.
//
// INTERRUPTS. There is no interrupt delivery in this runtime, so the cause register reads
// as "nothing pending". That is true rather than convenient: no device here raises one.

#include "mmio.h"

#include <lucent/log.h>

#include <cstdlib>
#include <cstring>

namespace {

constexpr u32 PI_BASE = 0xCC003000;
constexpr u32 PI_SIZE = 0x40;

constexpr u32 PI_INTSR       = PI_BASE + 0x00;   // interrupt cause
constexpr u32 PI_INTMR       = PI_BASE + 0x04;   // interrupt mask
constexpr u32 PI_FIFO_BASE   = PI_BASE + 0x0C;
constexpr u32 PI_FIFO_END    = PI_BASE + 0x10;
constexpr u32 PI_FIFO_WPTR   = PI_BASE + 0x14;
constexpr u32 PI_FLIPPER_REV = PI_BASE + 0x2C;

u32 g_reg[PI_SIZE / 4];

u32& reg(u32 addr) {
    const u32 i = (addr - PI_BASE) >> 2;
    if (i >= PI_SIZE / 4) {
        lucent::error("pi", "register index out of range for address 0x{:08x}", addr);
        std::abort();
    }
    return g_reg[i];
}

u32 pi_read(u32 ea, unsigned width) {
    if (width != 4) {
        lucent::error("pi", "unsupported {}-byte read @ 0x{:08x}", width, ea);
        std::abort();
    }
    return reg(ea & ~3u);
}

void pi_write(u32 ea, unsigned width, u32 value) {
    if (width != 4) {
        lucent::error("pi", "unsupported {}-byte write @ 0x{:08x}", width, ea);
        std::abort();
    }
    reg(ea & ~3u) = value;
}

} // namespace

void pi_device_init() {
    std::memset(g_reg, 0, sizeof(g_reg));
    reg(PI_INTSR)       = 0;   // nothing pending; no device here raises an interrupt
    reg(PI_INTMR)       = 0;
    reg(PI_FLIPPER_REV) = 0;   // guest falls back to its own "revision unknown" constant
    (void)PI_FIFO_BASE; (void)PI_FIFO_END; (void)PI_FIFO_WPTR;
    mmio_register(MmioDevice{PI_BASE, PI_BASE + PI_SIZE, "pi", &pi_read, &pi_write});
}
