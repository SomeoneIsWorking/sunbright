// dev_gxregs.cpp — CP (command processor) and PE (pixel engine) register blocks.
//
// These are the GX pipeline's control registers: CP owns the FIFO pointers and the
// read/write watermarks, PE owns z/alpha configuration and the draw-sync token.
//
// Neither is a renderer. Actual GX command traffic goes through the write-gather pipe at
// 0xCC008000 and is destined for aurora; these blocks are the configuration around it, and
// the SDK reads them back during GXInit. Modelling them as storage lets GXInit run for real
// rather than being overridden.
//
// Interrupt-status bits stay clear: nothing in this runtime raises a CP or PE interrupt.
// GXDrawDone is overridden precisely because its PE token interrupt has no source here.

#include "mmio.h"

#include <lucent/log.h>

#include <cstdlib>
#include <cstring>

namespace {

constexpr u32 CP_BASE = 0xCC000000, CP_SIZE = 0x80;
constexpr u32 PE_BASE = 0xCC001000, PE_SIZE = 0x40;

u16 g_cp[CP_SIZE / 2];
u16 g_pe[PE_SIZE / 2];

u16& slot(u16* bank, u32 base, u32 size, u32 addr, const char* who) {
    const u32 i = (addr - base) >> 1;
    if (i >= size / 2) {
        lucent::error(who, "register index out of range for address 0x{:08x}", addr);
        std::abort();
    }
    return bank[i];
}

u32 rd16(u16* bank, u32 base, u32 size, u32 ea, unsigned width, const char* who) {
    if (width == 2) return slot(bank, base, size, ea & ~1u, who);
    if (width == 4)
        return ((u32)slot(bank, base, size, ea, who) << 16) |
               slot(bank, base, size, ea + 2, who);
    lucent::error(who, "unsupported {}-byte read @ 0x{:08x}", width, ea);
    std::abort();
}

void wr16(u16* bank, u32 base, u32 size, u32 ea, unsigned width, u32 v, const char* who) {
    if (width == 2) { slot(bank, base, size, ea & ~1u, who) = (u16)v; return; }
    if (width == 4) {
        slot(bank, base, size, ea, who)     = (u16)(v >> 16);
        slot(bank, base, size, ea + 2, who) = (u16)v;
        return;
    }
    lucent::error(who, "unsupported {}-byte write @ 0x{:08x}", width, ea);
    std::abort();
}

u32  cp_read (u32 ea, unsigned w)          { return rd16(g_cp, CP_BASE, CP_SIZE, ea, w, "cp"); }
void cp_write(u32 ea, unsigned w, u32 v)   { wr16(g_cp, CP_BASE, CP_SIZE, ea, w, v, "cp"); }
u32  pe_read (u32 ea, unsigned w)          { return rd16(g_pe, PE_BASE, PE_SIZE, ea, w, "pe"); }
void pe_write(u32 ea, unsigned w, u32 v)   { wr16(g_pe, PE_BASE, PE_SIZE, ea, w, v, "pe"); }

} // namespace

void gxregs_device_init() {
    std::memset(g_cp, 0, sizeof(g_cp));
    std::memset(g_pe, 0, sizeof(g_pe));
    mmio_register(MmioDevice{CP_BASE, CP_BASE + CP_SIZE, "cp", &cp_read, &cp_write});
    mmio_register(MmioDevice{PE_BASE, PE_BASE + PE_SIZE, "pe", &pe_read, &pe_write});
}
