#pragma once
// ngx_mesh — N4 (docs/native_port_plan.md §3): assemble a native, renderer-facing
// mesh from a GameCube GX DRAW primitive, Dolphin-free. This is the layer above
// ngx_vertex: where ngx_vertex reads one attribute array, ngx_mesh extracts ALL
// present attributes per vertex (via the ngx_vertex extractors), interleaves them
// into a native vertex, and triangulates the GX primitive type (quads / strips /
// fans / lists) into a triangle-index list — i.e. exactly what a native GPU
// vertex+index buffer needs.
//
// It owns no GX framing of its own: the caller (the J3D shape hook, or a frame
// walk) supplies the active CP descriptor, the primitive opcode, the vertex
// count, and a host pointer to the primitive's vertex block — plus the resolved
// host pointers/strides of the indexed vertex arrays (CP ARRAY_BASE/STRIDE).
//
// Pure/offline: operates on host byte pointers; unit-testable with hand-built
// primitives (no running game, no GPU).

#include "ngx_decode.h"   // NgxCP
#include <vector>

// A native mesh vertex assembled from GC attributes. Only the attributes present
// in the CP descriptor are meaningful; absent ones are zeroed (color alpha 255).
struct NgxVertex {
    float pos[3];
    float nrm[3];
    unsigned char clr[2][4];   // color0, color1 (RGBA8)
    float tex[8][2];           // tex0..7 (S,T)
    unsigned char matidx;      // PNMTXIDX: position-matrix-memory row (GX_PNMTX0=0,1=3,…); 0 if absent
    unsigned char packet;      // which J3DShape matrix-packet (mMatrices[i]) drew this vertex; set by
                               // the capture loop. Skinned shapes draw in N packets, each reloading the
                               // pos-matrix slots with its OWN unkC table → the per-vertex matrix must
                               // be resolved against THIS packet's table, not a global last-writer.
};

// Resolved guest vertex arrays for indexed attributes (host pointers + byte
// strides). A null pointer means "not indexed / not present"; Direct attributes
// ignore these. Mirrors the CP ARRAY_BASE (0xA0+) / ARRAY_STRIDE (0xB0+) state.
struct NgxArrays {
    const unsigned char* pos = nullptr;       unsigned pos_stride = 0;
    const unsigned char* nrm = nullptr;       unsigned nrm_stride = 0;
    const unsigned char* clr[2] = {nullptr, nullptr};  unsigned clr_stride[2] = {0, 0};
    const unsigned char* tex[8] = {};          unsigned tex_stride[8] = {};
};

// Assemble one GX DRAW primitive into native vertices + triangle indices.
//   `op`    : primitive opcode 0x80..0xBF (prim type = op&0xF8, vat = op&7).
//   `vtx`   : host pointer to the primitive's first vertex.
//   `count` : vertex count.
//   `arr`   : resolved indexed-array pointers/strides.
// Appends `count` NgxVertex to `verts` and the triangulated indices (referencing
// the appended vertices, with the existing verts.size() as base) to `indices`.
// Triangulation: Quads→2 tris each, Tris→lists, TriStrip→alternating winding,
// TriFan→(0,i-1,i). Line/Point prims emit no triangles. Returns triangles emitted.
int ngx_assemble_primitive(const NgxCP& cp, unsigned op,
                           const unsigned char* vtx, int count,
                           const NgxArrays& arr,
                           std::vector<NgxVertex>& verts,
                           std::vector<unsigned>& indices);

// Resolve a guest address to a host byte pointer (or null if out of range). The
// live J3D hook passes the memory-bridge resolver; the self-test passes an
// identity over a flat buffer. Called for each indexed vertex array.
typedef const unsigned char* (*NgxHostResolve)(unsigned guest_addr, void* user);

// Build a native mesh from a J3D shape display list. Walks `dl` (the GX primitive
// stream — J3DShapeDraw::mDisplayList), resolving each present attribute's vertex
// array (cp.array_base/stride → host pointer via `resolve`) and assembling every
// DRAW primitive into `verts` + triangulated `indices`. `cp` is the CP state the
// shape's GD-command list programmed (VCD/VAT/array bases); it is copied so the
// walk's CP updates don't leak out. Returns total triangles emitted, or -1 if the
// stream failed to frame cleanly to the end.
int ngx_build_mesh(const NgxCP& cp, const unsigned char* dl, size_t dl_size,
                   NgxHostResolve resolve, void* resolve_user,
                   std::vector<NgxVertex>& verts, std::vector<unsigned>& indices);

// Self-test: hand-built primitives (quad/strip/fan/list) with known attributes +
// a display-list build with indexed-array resolution. Returns failing-case count.
int sb_ngx_mesh_selftest(char* out, int cap);
