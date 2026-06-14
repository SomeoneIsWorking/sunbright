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

// Extract `count` vertex normals into `out` (3 floats per vertex — the N vector;
// when the format is NTB we take N only). Dequant matches the GC normal
// convention (signed/unsigned fixed point by component format; cross-checked vs
// Dolphin VertexLoader_Normal FracAdjust). `nrm_array`/`nrm_stride` give the
// normal vertex array for Index8/16 (null for Direct). Returns normals written.
int ngx_extract_normals(float* out, const NgxCP& cp, unsigned vat,
                        const unsigned char* vtx, int count, unsigned vstride,
                        const unsigned char* nrm_array, unsigned nrm_stride);

// Extract `count` colors of channel `chan` (0 or 1) into `out` (4 bytes RGBA per
// vertex, 0..255). Unpacks all GC color formats (RGB565/RGB888/RGB888x/RGBA4444/
// RGBA6666/RGBA8888; missing alpha → 255). `col_array`/`col_stride` give the
// color vertex array for Index8/16. Returns colors written.
int ngx_extract_colors(unsigned char* out, const NgxCP& cp, unsigned vat, int chan,
                       const unsigned char* vtx, int count, unsigned vstride,
                       const unsigned char* col_array, unsigned col_stride);

// Extract `count` tex coords of channel `chan` (0..7) into `out` (2 floats S,T
// per vertex; S-only formats write t=0). Fixed-point dequant by the channel's
// Frac field (same pipeline as position components). `tex_array`/`tex_stride`
// give the tex vertex array for Index8/16. Returns coords written.
int ngx_extract_texcoords(float* out, const NgxCP& cp, unsigned vat, int chan,
                          const unsigned char* vtx, int count, unsigned vstride,
                          const unsigned char* tex_array, unsigned tex_stride);

// Self-test: hand-constructed inputs with known expected positions across the
// position formats/classes. Returns failing-case count (0 = OK); writes a report.
int sb_ngx_vertex_selftest(char* out, int cap);
