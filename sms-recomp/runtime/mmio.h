// mmio.h — memory-mapped device routing for the standalone recomp.
//
// Anything sb_ram_fast() rejects is either a hardware register or a genuine bad access.
// Devices claim an address range here; everything unclaimed stays LOUD (see
// slow_report) rather than returning a silent zero, because a silent zero reads as
// valid hardware state and fakes plausible-but-wrong behaviour for a long time.
//
// Widths are in BYTES and are the guest's access width. A device must handle every
// width the guest actually uses on it — the GC SDK frequently reads a 32-bit register
// as two halfwords, and splitting/merging is the device's business, not the router's.

#pragma once

#include "cpu_state.h"

struct MmioDevice {
    u32 lo;                 // inclusive
    u32 hi;                 // exclusive
    const char* name;
    u32  (*read )(u32 ea, unsigned width);
    void (*write)(u32 ea, unsigned width, u32 value);
};

void mmio_register(const MmioDevice& dev);

// Return false when no device claims the address, leaving the caller to report it.
bool mmio_read (u32 ea, unsigned width, u32& out);
bool mmio_write(u32 ea, unsigned width, u32 value);
