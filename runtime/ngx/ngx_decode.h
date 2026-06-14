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
    // CP vertex-array state (guest addresses / byte strides). Array index = CPArray
    // enum: Position=0, Normal=1, Color0=2, Color1=3, TexCoord0..7=4..11,
    // XF_A pos-mtx=12, XF_B nrm-mtx=13. Reg 0xA0+i = base, 0xB0+i = stride.
    u32 array_base[16] = {};
    u32 array_stride[16] = {};
};

// One DRAW primitive surfaced during a stream walk: opcode (0x80..0xBF; prim
// type = op&0xF8, vat = op&7), vertex count, and a host pointer to the
// primitive's vertex block.
struct NgxPrim { unsigned op; int count; const unsigned char* vtx; };

// Vertex attribute identifiers, in GC vertex-layout order (matrix-index bytes
// precede NGX_POS and are folded into NGX_POS's offset). NGX_ATTR_END is the
// one-past-tex7 marker = the full vertex size.
enum NgxAttr {
    NGX_POS = 0, NGX_NRM, NGX_CLR0, NGX_CLR1,
    NGX_TEX0, NGX_TEX1, NGX_TEX2, NGX_TEX3, NGX_TEX4, NGX_TEX5, NGX_TEX6, NGX_TEX7,
    NGX_ATTR_END
};

// Byte offset of `attr` within a vertex under the current CP descriptor (sum of
// the sizes of all preceding attributes). NGX_POS's offset = the matrix-index
// bytes; NGX_ATTR_END returns the full vertex size. Shares the size tables with
// ngx_vertex_size so layout truth lives in one place.
u32 ngx_attr_offset(const NgxCP& cp, unsigned vat, int attr);

// Vertex size in bytes for the given VAT under the current CP descriptor.
// Mirrors VertexLoaderBase::GetVertexSize exactly.
u32 ngx_vertex_size(const NgxCP& cp, unsigned vat);

// Parse `n` bytes of one frame's GP-FIFO stream; fills `out` (reset inside).
// Uses+advances the persistent global CP state. Returns out.ok (parsed exactly
// to the end with no unknown opcode — same verdict as gxp_parse_frame).
bool ngx_parse_frame(const u8* p, size_t n, GxFrameInfo& out);

// Walk a GX command stream (e.g. a J3D shape display list), updating `cp` on CP
// register loads (incl. array base/stride) and invoking `on_prim` for every
// DRAW primitive. Same framing as ngx_parse_frame. Returns true if the stream
// framed cleanly to the end (no truncation / unknown opcode).
bool ngx_walk_stream(const u8* p, size_t n, NgxCP& cp,
                     void (*on_prim)(const NgxPrim&, void*), void* user);

// Test/diagnostic access to the persistent CP state (parity harness, unit tests).
NgxCP& ngx_cp_state();
