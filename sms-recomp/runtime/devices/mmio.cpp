// mmio.cpp — the device router. See mmio.h.

#include "mmio.h"

#include <lucent/log.h>

#include <deque>

namespace {

// A handful of devices, consulted on every unrouted access. The write-gather pipe is MMIO too and
// receives every GX command word, so repeated access to one device is a hot path. Keep the general
// range registry, but remember the last device per thread before falling back to its linear scan.
std::deque<MmioDevice>& devices() {
    static std::deque<MmioDevice> d;
    return d;
}

const MmioDevice* find(u32 ea) {
    static thread_local const MmioDevice* last = nullptr;
    if (last != nullptr && ea >= last->lo && ea < last->hi)
        return last;
    for (const auto& d : devices())
        if (ea >= d.lo && ea < d.hi) {
            last = &d;
            return last;
        }
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
    if (const MmioDevice* d = find(ea)) {
        out = d->read(ea, width);
        return true;
    }
    return false;
}

bool mmio_write(u32 ea, unsigned width, u32 value) {
    if (const MmioDevice* d = find(ea)) {
        d->write(ea, width, value);
        return true;
    }
    return false;
}
