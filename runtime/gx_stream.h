#pragma once
// GX stream assembler — milestone A of the 60 fps interpolation arc
// (docs/model_interpolation.md): own the GX command stream.
//
// When armed (SUNBRIGHT_GXOWN=1 and the first display copy has been seen),
// every gather-pipe byte is appended to a host-side buffer instead of going to
// Dolphin's GPFifo directly; whole runs are flushed at the guest's own sync
// points (GXFlush / GXCopyDisp — see runtime/overrides/gx_stream_own.cpp).
// Semantics are unchanged at this stage; what it buys is a complete, bounded,
// frame-delimited copy of each frame's command stream — the substrate the
// interpolation replay (re-push of frame N-1 with blended draw matrices) and
// the native-renderer arc both stand on.
//
// Threading invariant: the gather pipe is written only by guest threads, which
// nthr serializes (one runs at a time), and every flush trigger is also on a
// guest thread — so the buffer needs no lock.

#include "cpu_state.h"

// Routing switch: env SUNBRIGHT_GXOWN=1 AND armed (first GXCopyDisp seen, so
// boot-time writes from JIT-routed GX init never interleave with held bytes).
bool gxs_active();
void gxs_arm();                       // called at the first display copy
bool gxs_in_flush();                  // true while gxs_flush drains into GPFifo
                                      // (lets the GPFifo --wrap pass our own
                                      // writes through; see gpfifo_wrap.cpp)

// Append one gather-pipe write (values exactly as the guest wrote them).
void gxs_w8(u8 v);
void gxs_w16(u16 v);
void gxs_w32(u32 v);
void gxs_w64(u64 v);

// Burst the held bytes into Dolphin's GPFifo (in order). `why` feeds the
// SUNBRIGHT_DBG_GXS stats.
void gxs_flush(const char* why);

// Frame boundary: GXCopyDisp completed (its bytes are in the buffer/FIFO).
// Flushes and rotates the per-frame capture.
void gxs_frame_boundary();
