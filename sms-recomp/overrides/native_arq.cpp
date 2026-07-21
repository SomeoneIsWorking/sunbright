// native_arq.cpp — ARQ (the ARAM DMA request queue) completes synchronously.
//
// Retail queues an ARQRequest, the ARAM DMA engine runs it, and the ARAM interrupt dequeues
// the next request and invokes the completion callback. This runtime has no interrupt
// source, so a queued request is never completed and callers spin: JASystem::Kernel::
// portCmdInit posts requests and then waits on a pending count that never drains, which
// parked the audio kernel thread and, through priority preemption, the whole boot.
//
// The transfer itself has nothing to wait for — ARAM is a host buffer and the copy is a
// memcpy. So the request is performed and completed inside the post, which is the same
// "synchronous unthrottled I/O" model used for ARAM at the register level and for DVD.

#include "overrides.h"

#include <aram.h>
#include <intrinsics.h>
#include <lucent/log.h>

extern void call_ppc(CPUState& cpu, u32 address);

namespace {

// ARQRequest layout, from the recompiled ARQPostRequest (0x80353bdc) prologue stores:
//   stw r0,0(r3) / r4,4 / r5,8 / r7,0x10 / r8,0x14 / r9,0x18 / r10,0x1c
constexpr u32 RQ_NEXT     = 0x00;
constexpr u32 RQ_OWNER    = 0x04;
constexpr u32 RQ_TYPE     = 0x08;
constexpr u32 RQ_SOURCE   = 0x10;
constexpr u32 RQ_DEST     = 0x14;
constexpr u32 RQ_LENGTH   = 0x18;
constexpr u32 RQ_CALLBACK = 0x1C;

// ARQ transfer types: 0 = main memory -> ARAM, 1 = ARAM -> main memory.
constexpr u32 ARQ_TYPE_MRAM_TO_ARAM = 0;

// ARQPostRequest(ARQRequest* r3, u32 owner r4, u32 type r5, u32 priority r6,
//                u32 source r7, u32 dest r8, u32 length r9, ARQCallback callback r10)
void arq_post_request(CPUState& cpu) {
    const u32 req      = cpu.gpr[3];
    const u32 owner    = cpu.gpr[4];
    const u32 type     = cpu.gpr[5];
    const u32 source   = cpu.gpr[7];
    const u32 dest     = cpu.gpr[8];
    const u32 length   = cpu.gpr[9];
    const u32 callback = cpu.gpr[10];

    // Fill the request exactly as retail does — callbacks and callers read these back.
    sb_w32(req + RQ_NEXT, 0);
    sb_w32(req + RQ_OWNER, owner);
    sb_w32(req + RQ_TYPE, type);
    sb_w32(req + RQ_SOURCE, source);
    sb_w32(req + RQ_DEST, dest);
    sb_w32(req + RQ_LENGTH, length);
    sb_w32(req + RQ_CALLBACK, callback);

    // type selects the direction; source and dest are already the right way round for it.
    if (type == ARQ_TYPE_MRAM_TO_ARAM) aram_dma(source, dest, length, /*to_mram=*/false);
    else                               aram_dma(dest, source, length, /*to_mram=*/true);

    lucent::debug("arq", "{} len 0x{:x} src 0x{:08x} dst 0x{:08x}",
                  type == ARQ_TYPE_MRAM_TO_ARAM ? "MM->AR" : "AR->MM", length, source, dest);

    // Completion callback, as the ARAM interrupt would have invoked it: r3 = the request.
    // Whole-state save/restore so the caller sees an ordinary ABI-respecting call — this is
    // a nested call inside a function that has not returned yet.
    if (callback) {
        const CPUState saved = cpu;
        cpu.gpr[3] = req;
        call_ppc(cpu, callback);
        cpu = saved;
    }
}

} // namespace

SB_OVERRIDE(0x80353bdcu, arq_post_request, "ARQPostRequest",
            "no ARAM interrupt exists to complete a queued request; the copy is a memcpy")
