// dev_vi.cpp — VI (video interface) registers.
//
// VI is the display controller: timing registers, the framebuffer base, filter
// coefficients, and a few status registers the SDK probes at init. This port does not scan
// out through VI at all — aurora presents — so the registers are configuration the guest
// writes and reads back, plus the small number of them that report something real.
//
// Frame PACING does not come from here: VIWaitForRetrace is overridden as a pure counter
// (overrides/native_vi.cpp), because there is no retrace interrupt source.

#include "mmio.h"

#include <lucent/log.h>

#include <cstdlib>
#include <cstring>

namespace {

constexpr u32 VI_BASE = 0xCC002000;
constexpr u32 VI_SIZE = 0x80;

// Component-cable / progressive-scan detect. Retail reads this to decide whether to offer
// progressive mode. Reporting "not connected" selects the ordinary interlaced NTSC path,
// which is the mode the decomp runtime renders and the oracle captures — so this keeps the
// two runtimes comparable rather than silently diverging on video mode.
constexpr u32 VI_DTV_STATUS = VI_BASE + 0x6C;

u16 g_reg[VI_SIZE / 2];

// Takes a full guest ADDRESS, not a bare offset — passing an offset here underflows the
// index and corrupts memory outside the array (it did, before VI_DTV_STATUS was fixed).
u16& reg(u32 addr) {
    const u32 i = (addr - VI_BASE) >> 1;
    if (i >= VI_SIZE / 2) {
        lucent::error("vi", "register index out of range for address 0x{:08x}", addr);
        std::abort();
    }
    return g_reg[i];
}

u32 vi_read(u32 ea, unsigned width) {
    if (width == 2) return reg(ea & ~1u);
    if (width == 4) return ((u32)reg(ea) << 16) | reg(ea + 2);
    lucent::error("vi", "unsupported {}-byte read @ 0x{:08x}", width, ea);
    std::abort();
}

void vi_write(u32 ea, unsigned width, u32 value) {
    if (width == 2) { reg(ea & ~1u) = (u16)value; return; }
    if (width == 4) { reg(ea) = (u16)(value >> 16); reg(ea + 2) = (u16)value; return; }
    lucent::error("vi", "unsupported {}-byte write @ 0x{:08x}", width, ea);
    std::abort();
}

} // namespace

void vi_device_init() {
    std::memset(g_reg, 0, sizeof(g_reg));
    reg(VI_DTV_STATUS) = 0;   // no component cable attached
    mmio_register(MmioDevice{VI_BASE, VI_BASE + VI_SIZE, "vi", &vi_read, &vi_write});
}
