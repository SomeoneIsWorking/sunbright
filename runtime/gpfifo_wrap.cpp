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

// Original Dolphin bodies, exposed by the fork (replace the --wrap __real_).
extern "C" {
void sb_gpfifo_write8_impl(void* self, u8 v);
void sb_gpfifo_write16_impl(void* self, u16 v);
void sb_gpfifo_write32_impl(void* self, u32 v);
void sb_gpfifo_write64_impl(void* self, u64 v);

// Fork hooks (installed via sb_install_hooks → sb_hook_gpfifo_write*). Same bodies as the old
// __wrap_; call the *_impl to run Dolphin's original path.
void sb_hook_gpfifo_write8(void* self, u8 v) {
    if (gxs_active() && !gxs_in_flush()) { note(8); gxs_w8(v); return; }
    sb_gpfifo_write8_impl(self, v);
}
void sb_hook_gpfifo_write16(void* self, u16 v) {
    if (gxs_active() && !gxs_in_flush()) { note(16); gxs_w16(v); return; }
    sb_gpfifo_write16_impl(self, v);
}
void sb_hook_gpfifo_write32(void* self, u32 v) {
    if (gxs_active() && !gxs_in_flush()) { note(32); gxs_w32(v); return; }
    sb_gpfifo_write32_impl(self, v);
}
void sb_hook_gpfifo_write64(void* self, u64 v) {
    if (gxs_active() && !gxs_in_flush()) { note(64); gxs_w64(v); return; }
    sb_gpfifo_write64_impl(self, v);
}
}  // extern "C"
