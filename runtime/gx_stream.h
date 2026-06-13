#pragma once
// GX stream assembler — the tailored renderer's frontend (gx_stream.cpp):
// own the GX command stream. This is unconditional now (no env gate) — both
// vanilla and 60 fps run through it.
//
// Once armed (the first display copy has been seen — so boot-time writes from
// JIT-routed GX init never interleave with held bytes), every gather-pipe byte
// is appended to a host-side buffer instead of going to Dolphin's GPFifo
// directly; whole runs are flushed at the guest's own sync points (GXFlush /
// GXCopyDisp — see runtime/overrides/gx_stream_own.cpp) and decoded
// synchronously through Dolphin's OpcodeDecoder — no CP ring, so a GatherPipe
// overflow is structurally impossible (this is what makes the 60 fps redraw's
// doubled command volume safe). It also yields a complete, frame-delimited copy
// of each frame's command stream for the interpolation arc.
//
// Threading invariant: the gather pipe is written only by guest threads, which
// nthr serializes (one runs at a time), and every flush trigger is also on a
// guest thread — so the buffer needs no lock.

#include "cpu_state.h"

bool gxs_active();                    // true once armed (first display copy seen)
void gxs_arm();                       // called at the first display copy
bool gxs_in_flush();                  // true while gxs_flush is decoding (lets the
                                      // GPFifo --wrap pass our own writes through;
                                      // see gpfifo_wrap.cpp)

// Append one gather-pipe write (values exactly as the guest wrote them).
void gxs_w8(u8 v);
void gxs_w16(u16 v);
void gxs_w32(u32 v);
void gxs_w64(u64 v);

// Decode the held bytes synchronously through the OpcodeDecoder (in order).
// `why` feeds the SUNBRIGHT_DBG_GXS stats.
void gxs_flush(const char* why);

// Frame boundary: GXCopyDisp completed (its bytes are in the buffer/FIFO).
// Flushes and rotates the per-frame capture.
void gxs_frame_boundary();
