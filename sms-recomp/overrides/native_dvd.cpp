// native_dvd.cpp — DVD reads become instant filesystem reads.
//
// The drive is gone. Retail queues a command, the DI hardware DMAs, an interrupt fires, and
// a callback runs; this port reads the bytes out of the disc image inside the call and runs
// the callback before returning. There is no drive protocol, no command queue to drain, no
// interrupt to deliver, and no rotational latency to model.
//
// This replaces the DI-register path for file I/O. Modelling the drive was working — the
// disc mounted and served the inquiry — but it meant reproducing hardware whose only purpose
// was to hide latency the host does not have, and it dragged in interrupt delivery, command
// queueing and a drive-identification block whose retail contents this port cannot verify.
// The game's DVD library rejected that unverifiable drive ID and retried forever. Cutting
// above the protocol removes all of it at once.
//
// The FST, path resolution, file handles and every DVD*/JKRDvdFile caller above this point
// still run as recompiled PPC. Only the transport is replaced.

#include "overrides.h"

#include <disc.h>
#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdlib>
#include <vector>

extern u8* g_ram_base;
extern void call_ppc(CPUState& cpu, u32 address);

namespace {

// DVDCommandBlock layout, read off the recompiled DVDReadAbsAsyncPrio (0x8034da6c) and
// DVDInquiryAsync (0x8034dfe4):
//   stw r5,20(r3) / stw r4,24(r3) / stw r6,16(r3) / stw r0,32(r3) / stw r7,40(r3)
// and off DVDGetCommandBlockStatus (0x8034e0f8): lwz r0,12(r31).
constexpr u32 CB_COMMAND     = 0x08;
constexpr u32 CB_STATE       = 0x0C;
constexpr u32 CB_OFFSET      = 0x10;   // absolute byte offset on the disc
constexpr u32 CB_LENGTH      = 0x14;
constexpr u32 CB_ADDR        = 0x18;   // destination in main memory
constexpr u32 CB_TRANSFERRED = 0x20;
constexpr u32 CB_CALLBACK    = 0x28;

constexpr u32 DVD_STATE_END = 0;

// Run the guest's completion callback as a nested call. The whole CPU state is saved and
// restored so the caller of our override sees an ordinary ABI-respecting call: the callback
// may clobber whatever it likes, exactly as it could when the interrupt handler invoked it.
// Guest MEMORY changes persist, as they must.
void run_callback(CPUState& cpu, u32 callback, s32 result, u32 block) {
    if (!callback) return;
    const CPUState saved = cpu;
    cpu.gpr[3] = (u32)result;
    cpu.gpr[4] = block;
    call_ppc(cpu, callback);
    cpu = saved;
}

// Copy disc bytes straight into guest memory.
void read_into_guest(u64 offset, u32 guest_addr, u32 length) {
    const u32 off = guest_addr & 0x01FFFFFFu;
    if (off + length > 0x01800000u) {
        lucent::error("dvd", "read target 0x{:08x} +0x{:x} is outside MEM1", guest_addr,
                      length);
        std::abort();
    }
    disc_read(offset, g_ram_base + off, length);
}

// DVDReadAbsAsyncPrio(DVDCommandBlock* r3, void* addr r4, s32 length r5, s32 offset r6,
//                     DVDCBCallback callback r7, s32 prio r8) -> BOOL
void dvd_read_abs_async_prio(CPUState& cpu) {
    const u32 block    = cpu.gpr[3];
    const u32 addr     = cpu.gpr[4];
    const s32 length   = (s32)cpu.gpr[5];
    const s32 offset   = (s32)cpu.gpr[6];
    const u32 callback = cpu.gpr[7];

    if (length < 0 || offset < 0) {
        lucent::error("dvd", "negative read: length {} offset {}", length, offset);
        std::abort();
    }

    // Keep the block exactly as retail leaves it — callers read these fields back.
    sb_w32(block + CB_COMMAND, 1);              // DVD_COMMAND_READ
    sb_w32(block + CB_OFFSET, (u32)offset);
    sb_w32(block + CB_LENGTH, (u32)length);
    sb_w32(block + CB_ADDR, addr);
    sb_w32(block + CB_CALLBACK, callback);

    read_into_guest((u64)(u32)offset, addr, (u32)length);

    sb_w32(block + CB_TRANSFERRED, (u32)length);
    sb_w32(block + CB_STATE, DVD_STATE_END);

    lucent::debug("dvd", "read 0x{:x} bytes @ 0x{:x} -> 0x{:08x}", length, offset, addr);

    run_callback(cpu, callback, length, block);
    cpu.gpr[3] = 1;                             // TRUE
}

// DVDInquiryAsync(DVDCommandBlock* r3, DVDDriveInfo* r4, DVDCBCallback r5) -> BOOL
//
// There is no drive to interrogate. Retail uses the reply to pick firmware workarounds, so
// reporting a zeroed block is both truthful (no drive) and inert (no workaround selected).
// This is what the DI-level placeholder could not achieve: down there the library saw an
// implausible drive and retried forever.
void dvd_inquiry_async(CPUState& cpu) {
    const u32 block    = cpu.gpr[3];
    const u32 info     = cpu.gpr[4];
    const u32 callback = cpu.gpr[5];

    for (u32 i = 0; i < 32; i += 4) sb_w32(info + i, 0);

    sb_w32(block + CB_COMMAND, 14);             // DVD_COMMAND_INQUIRY
    sb_w32(block + CB_LENGTH, 32);
    sb_w32(block + CB_ADDR, info);
    sb_w32(block + CB_CALLBACK, callback);
    sb_w32(block + CB_TRANSFERRED, 32);
    sb_w32(block + CB_STATE, DVD_STATE_END);

    run_callback(cpu, callback, 32, block);
    cpu.gpr[3] = 1;
}

// DVDReadDiskID(DVDCommandBlock* r3, DVDDiskID* r4, DVDCBCallback r5) -> BOOL
// The disk ID is the first 32 bytes of the disc, which we do have.
void dvd_read_disk_id(CPUState& cpu) {
    const u32 block    = cpu.gpr[3];
    const u32 id       = cpu.gpr[4];
    const u32 callback = cpu.gpr[5];

    read_into_guest(0, id, 32);

    sb_w32(block + CB_COMMAND, 5);              // DVD_COMMAND_READ_ID
    sb_w32(block + CB_LENGTH, 32);
    sb_w32(block + CB_ADDR, id);
    sb_w32(block + CB_CALLBACK, callback);
    sb_w32(block + CB_TRANSFERRED, 32);
    sb_w32(block + CB_STATE, DVD_STATE_END);

    run_callback(cpu, callback, 32, block);
    cpu.gpr[3] = 1;
}

} // namespace

SB_OVERRIDE(0x8034da6cu, dvd_read_abs_async_prio, "DVDReadAbsAsyncPrio",
            "instant read from the disc image; no drive, no queue, no interrupt")
SB_OVERRIDE(0x8034dfe4u, dvd_inquiry_async, "DVDInquiryAsync",
            "no drive to interrogate; a zeroed reply selects no firmware workaround")
SB_OVERRIDE(0x8034dc18u, dvd_read_disk_id, "DVDReadDiskID",
            "the disk ID is the first 32 bytes of the disc image")
