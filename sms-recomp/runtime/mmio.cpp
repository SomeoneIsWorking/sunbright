// mmio.cpp — the device router. See mmio.h.

#include "mmio.h"

#include <lucent/log.h>

#include <vector>

namespace {

// A handful of devices, consulted on every unrouted access. Linear scan is fine and
// keeps registration order-independent; the hot path never reaches here because
// sb_ram_fast handles all real RAM inline.
std::vector<MmioDevice>& devices() {
    static std::vector<MmioDevice> d;
    return d;
}

const MmioDevice* find(u32 ea) {
    for (const auto& d : devices())
        if (ea >= d.lo && ea < d.hi) return &d;
    return nullptr;
}

} // namespace

void mmio_register(const MmioDevice& dev) {
    for (const auto& d : devices()) {
        // Overlapping ranges mean one device silently shadows another — the kind of
        // defect that presents as a device "not working" much later.
        if (dev.lo < d.hi && d.lo < dev.hi) {
            lucent::error("mmio", "device {} [0x{:08x},0x{:08x}) overlaps {} [0x{:08x},0x{:08x})",
                          dev.name, dev.lo, dev.hi, d.name, d.lo, d.hi);
            std::abort();
        }
    }
    devices().push_back(dev);
    lucent::info("mmio", "device {} claims [0x{:08x}, 0x{:08x})", dev.name, dev.lo, dev.hi);
}

bool mmio_read(u32 ea, unsigned width, u32& out) {
    if (const MmioDevice* d = find(ea)) { out = d->read(ea, width); return true; }
    return false;
}

bool mmio_write(u32 ea, unsigned width, u32 value) {
    if (const MmioDevice* d = find(ea)) { d->write(ea, width, value); return true; }
    return false;
}
