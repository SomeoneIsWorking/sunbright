// dev_mi.cpp — MI (memory interface): memory-protection ranges and its interrupt mask.
//
// MI programs which MEM1 regions raise an interrupt on access, which is how retail catches
// stray DMA during development. This port has no equivalent hardware and does not need one:
// rt_mem_init already mprotect(PROT_NONE)s everything past real MEM1, so a stray access
// faults on the host immediately. The registers are therefore configuration the guest
// writes and reads back, with no interrupt ever raised (nothing here can raise one).
//
// Registers in this block are 16-bit, unlike most of the other device blocks.

#include "mmio.h"

#include <lucent/log.h>

#include <cstdlib>
#include <cstring>

namespace {

constexpr u32 MI_BASE = 0xCC004000;
constexpr u32 MI_SIZE = 0x80;

u16 g_reg[MI_SIZE / 2];

u16& reg(u32 addr) {
    const u32 i = (addr - MI_BASE) >> 1;
    if (i >= MI_SIZE / 2) {
        lucent::error("mi", "register index out of range for address 0x{:08x}", addr);
        std::abort();
    }
    return g_reg[i];
}

u32 mi_read(u32 ea, unsigned width) {
    if (width == 2) return reg(ea & ~1u);
    if (width == 4) return ((u32)reg(ea) << 16) | reg(ea + 2);
    lucent::error("mi", "unsupported {}-byte read @ 0x{:08x}", width, ea);
    std::abort();
}

void mi_write(u32 ea, unsigned width, u32 value) {
    if (width == 2) { reg(ea & ~1u) = (u16)value; return; }
    if (width == 4) { reg(ea) = (u16)(value >> 16); reg(ea + 2) = (u16)value; return; }
    lucent::error("mi", "unsupported {}-byte write @ 0x{:08x}", width, ea);
    std::abort();
}

} // namespace

void mi_device_init() {
    std::memset(g_reg, 0, sizeof(g_reg));
    mmio_register(MmioDevice{MI_BASE, MI_BASE + MI_SIZE, "mi", &mi_read, &mi_write});
}
