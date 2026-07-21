// exi.h — attaching devices to the EXI bus.
//
// EXI is a transport (dev_exi.cpp); the things hanging off it — SRAM/RTC, memory cards,
// the IPL ROM — speak their own protocols. Each attaches to one (channel, chip-select)
// pair and sees only the transfers addressed to it.

#pragma once

#include "cpu_state.h"

struct ExiDevice {
    u32 channel;
    u32 device;             // chip-select line, 0..2
    const char* name;

    // Immediate transfer, up to 4 bytes, MSB-first / left-justified in the DATA register
    // exactly as the hardware presents it. `len` is the real byte count.
    void (*imm_write)(u32 value, u32 len);
    u32  (*imm_read )(u32 len);

    // DMA to/from guest main memory. `to_device` is true when the guest is writing.
    void (*dma)(u32 guest_addr, u32 len, bool to_device);
};

void exi_attach(const ExiDevice& dev);
