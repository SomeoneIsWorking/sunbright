// gx_wgfifo_stub.cpp — gather-pipe (WGPIPE) writes from reference/sms J3D.
//
// The GC SDK's write-gather pipe (`WGPIPE.u8 = x;` etc.) is shimmed in
// native/shim/dolphin/ to C entry points sb_gx_wgfifo_{u8,u16,u32,f32}. In
// the retired oracle sink they pushed into sb::gxfifo's byte buffer for
// Dolphin's OpcodeDecoder to drain. Post-purge (single-path, Aurora coming),
// they are no-ops — the low-level FIFO byte stream is not consumed anywhere.
//
// When Aurora is wired in as Path A (GC-faithful), these route into Aurora's
// command-processor FIFO instead. Until then, stubs. Marked with the STOPGAP
// convention because they're a hold-open, not a permanent design.

#include <cstdint>

extern "C" {

// STOPGAP: no-op wgfifo shims because Aurora bridge isn't wired yet.
// Proper fix: forward to aurora::gx::fifo when render_gc/ lands.
void sb_gx_wgfifo_u8 (unsigned char)  {}
void sb_gx_wgfifo_u16(unsigned short) {}
void sb_gx_wgfifo_u32(unsigned int)   {}
void sb_gx_wgfifo_f32(float)          {}

} // extern "C"
