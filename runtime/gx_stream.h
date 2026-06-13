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

// Instrumentation accessors (used by the 60 fps probe to measure whether the
// in-between field actually re-rendered, and what matrix buffers it loaded).
unsigned long long gxs_decoded_bytes();   // running total of bytes decoded
unsigned long      gxs_decode_runs();     // running count of decode_owned() runs
struct GxFrameInfo;
const GxFrameInfo& gxs_prev_frame_info(); // parse of the most-recently-completed frame

// Inter-present wall-time jitter stats (the objective frame-delivery measure).
void gxs_frametime_reset();
void gxs_frametime_stats(unsigned long* n, double* mean_ms, double* stddev_ms,
                         double* min_ms, double* max_ms);
void gxs_frametime_decode(double* mean_dec_ms, double* dec_at_max);
void gxs_frametime_ct(double* mean_ct_ms, double* ct_at_max);
double gxs_decode_tick_begin();
void   gxs_decode_tick_end(double t0);
double gxs_ct_tick_begin();
void   gxs_ct_tick_end(double t0);
