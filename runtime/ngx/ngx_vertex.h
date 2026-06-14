#pragma once
// ngx_vertex — N4 foundation (docs/native_port_plan.md §3): extract native mesh
// vertex attributes from GameCube vertex data, Dolphin-free. Where ngx_decode
// (R1) framed primitives + computed vertex SIZE, this reads the attribute VALUES
// (positions first) so J3D shape geometry becomes a native vertex buffer.
//
// Operates on host byte pointers (raw GC bytes, big-endian as stored): the
// primitive's vertex-data block, and the position array base (for indexed
// attributes). The J3D integration resolves guest addresses to host pointers;
// the self-test passes host buffers directly so it needs no running game.
//
// Mirrors the GC position pipeline (cross-checked vs Dolphin VertexLoader_Position
// at implementation time; nothing linked): component format u8/s8/u16/s16/float,
// XY/XYZ, fixed-point dequant by PosFrac, direct vs Index8/Index16.

#include "ngx_decode.h"   // NgxCP, VCD/VAT accessors

// Extract `count` vertex positions into `out` (3 floats per vertex; XY formats
// write z=0). `vtx` points at the primitive's first vertex; `vstride` is the full
// per-vertex byte size (ngx_vertex_size). `pos_array` is the host pointer to the
// position vertex array (Index8/16 only; may be null for Direct) and `pos_stride`
// its byte stride. Returns the number of positions written.
int ngx_extract_positions(float* out, const NgxCP& cp, unsigned vat,
                          const unsigned char* vtx, int count, unsigned vstride,
                          const unsigned char* pos_array, unsigned pos_stride);

// Self-test: hand-constructed inputs with known expected positions across the
// position formats/classes. Returns failing-case count (0 = OK); writes a report.
int sb_ngx_vertex_selftest(char* out, int cap);
