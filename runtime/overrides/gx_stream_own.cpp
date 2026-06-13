// GX stream assembler sync points — see runtime/gx_stream.h.
//
// The assembler holds gather-pipe bytes host-side; the guest's own pipeline sync
// points are where they must be decoded (synchronous OpcodeDecoder):
//   - GXFlush (0x8035d8f0): the GC contract is "after GXFlush the GPU can see
//     everything written so far" — every guest wait (drawsync pushBreakPoint,
//     GXDrawDone, PE-token polls) flushes first, so flushing here preserves all
//     ordering the game can observe.
//   - GXCopyDisp (0x8035ecec): the display copy = the frame boundary. Arms the
//     assembler on first sight (boot-time pipe writes from JIT-routed GX init
//     must never interleave with held bytes) and rotates the per-frame capture.
#include "../overrides.h"
#include "../gx_stream.h"

namespace {

extern "C" void func_8035d8f0(CPUState&);   // GXFlush
extern "C" void func_8035ecec(CPUState&);   // GXCopyDisp

SUNBRIGHT_OVERRIDE(ov_gxs_flush, 0x8035d8f0u) {
    func_8035d8f0(cpu);          // guest body: writes the 32-byte alignment pad
    gxs_flush("gxflush");
}

SUNBRIGHT_OVERRIDE(ov_gxs_copydisp, 0x8035ececu) {
    func_8035ecec(cpu);          // guest body: emits the EFB→XFB copy commands
    gxs_arm();
    gxs_frame_boundary();
}

}  // namespace
