// GPFifo write funnel completion (GX stream assembler, gx_stream.cpp).
//
// The assembler holds gather-pipe bytes host-side; correctness requires that
// NOTHING ELSE appends to Dolphin's gather pipe while bytes are held, or the
// two byte streams interleave and the CP desyncs (observed live: "Unknown
// Opcode 0x3f" within a frame of arming). Recomp writes funnel through
// memory_bridge, but guest code executed by Dolphin's interpreter (run_jit_sync
// single-steps every JIT-only function) reaches GPFifoManager::Write* directly
// via MMU::Write — this --wrap closes that path: while the assembler is armed,
// foreign Write* calls are appended to the held buffer (keeping stream order);
// otherwise they pass through. The assembler's own flush calls __real_*.
//
// Dolphin's JIT inline-gather optimization (gather_pipe_ptr bump, no call)
// would still bypass this seam — but JIT handoffs never run pipe-writing guest
// code here (the interpreter path is what call_ppc uses); the fill-delta
// detector in gx_stream.cpp stays on to falsify that assumption loudly.
#include "cpu_state.h"
#include "gx_stream.h"

#include <cstdio>

namespace {
bool note(int bits) {
    static unsigned long n = 0;
    if (++n <= 8 || (n & 0xFFFF) == 0)
        fprintf(stderr, "[gxs] foreign GPFifo Write%d routed into held stream (n=%lu)\n", bits, n);
    return true;
}
}

extern "C" {
void __real__ZN6GPFifo13GPFifoManager6Write8Eh(void* self, u8 v);
void __real__ZN6GPFifo13GPFifoManager7Write16Et(void* self, u16 v);
void __real__ZN6GPFifo13GPFifoManager7Write32Ej(void* self, u32 v);
void __real__ZN6GPFifo13GPFifoManager7Write64Em(void* self, u64 v);

void __wrap__ZN6GPFifo13GPFifoManager6Write8Eh(void* self, u8 v) {
    if (gxs_active() && !gxs_in_flush()) { note(8); gxs_w8(v); return; }
    __real__ZN6GPFifo13GPFifoManager6Write8Eh(self, v);
}
void __wrap__ZN6GPFifo13GPFifoManager7Write16Et(void* self, u16 v) {
    if (gxs_active() && !gxs_in_flush()) { note(16); gxs_w16(v); return; }
    __real__ZN6GPFifo13GPFifoManager7Write16Et(self, v);
}
void __wrap__ZN6GPFifo13GPFifoManager7Write32Ej(void* self, u32 v) {
    if (gxs_active() && !gxs_in_flush()) { note(32); gxs_w32(v); return; }
    __real__ZN6GPFifo13GPFifoManager7Write32Ej(self, v);
}
void __wrap__ZN6GPFifo13GPFifoManager7Write64Em(void* self, u64 v) {
    if (gxs_active() && !gxs_in_flush()) { note(64); gxs_w64(v); return; }
    __real__ZN6GPFifo13GPFifoManager7Write64Em(self, v);
}
}  // extern "C"
