#pragma once
// ngx_decode — R1 of the native-renderer plan (docs/native_port_plan.md §3a):
// our OWN GameCube GP-FIFO command decoder, with ZERO Dolphin dependency.
//
// This is the first Dolphin-free foundation stone of the GX-level native
// renderer. It re-derives, from scratch, the three things the renderer (and the
// interpolation arc) need out of a captured frame's gather-pipe byte stream:
//   - command framing (so every byte is accounted for, byte-exactly),
//   - persistent CP state (VCD/VAT/array bases) so primitive vertex sizes are
//     computed correctly across shapes/frames,
//   - the same forensics the existing analyzer produces (PE-token offsets, CP
//     array-base loads for the pos/nrm matrix arrays, prim/DL/copy counts).
//
// It fills the SAME GxFrameInfo as gx_parse.h's Dolphin-based gxp_parse_frame,
// so the two can be run in lockstep and compared field-by-field (the parity
// harness in gx_stream.cpp under SUNBRIGHT_NGX_PARITY). Goal: 100% byte-parity
// + identical counts over a long gameplay capture. Once proven, this replaces
// the Dolphin OpcodeDecoder/VertexLoader as the renderer frontend (R2+).
//
// All numbers (framing, size tables, VCD/VAT bit layout, BP/CP register ids)
// are transcribed directly from the GameCube GX spec as implemented in
// externals/dolphin VideoCommon (OpcodeDecoding.h, CPMemory.{h,cpp},
// VertexLoader_*.h, BPMemory.h) — see docs/native_port_plan.md §3a. No Dolphin
// headers are included here.

#include "gx_parse.h"   // GxFrameInfo (shared result struct)
#include <cstddef>

// Persistent CP state mirrored across frames (J3D reprograms VCD/VAT per shape;
// state converges within the first armed frame). One global instance is used by
// ngx_parse_frame, mirroring the oracle's persistent CPState.
struct NgxCP {
    u32 vcd_lo = 0;          // VCD low  (CP reg 0x50): matrix idx + pos/nrm/col classes
    u32 vcd_hi = 0;          // VCD high (CP reg 0x60): tex-coord classes
    u32 vat[8][3] = {};      // [vat][group] : group 0=VAT_A(0x70) 1=VAT_B(0x80) 2=VAT_C(0x90)
};

// Vertex size in bytes for the given VAT under the current CP descriptor.
// Mirrors VertexLoaderBase::GetVertexSize exactly.
u32 ngx_vertex_size(const NgxCP& cp, unsigned vat);

// Parse `n` bytes of one frame's GP-FIFO stream; fills `out` (reset inside).
// Uses+advances the persistent global CP state. Returns out.ok (parsed exactly
// to the end with no unknown opcode — same verdict as gxp_parse_frame).
bool ngx_parse_frame(const u8* p, size_t n, GxFrameInfo& out);

// Test/diagnostic access to the persistent CP state (parity harness, unit tests).
NgxCP& ngx_cp_state();
