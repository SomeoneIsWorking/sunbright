#pragma once
// ngx_render_data — the snapshot contract between the J3DShape capture
// (ngx_j3d_shape.cpp, producer) and the native Vulkan renderer (vk_mesh.cpp,
// consumer). The capture records, per frame's-worth of shapes, a flat list of
// clip-space vertices plus a list of draw BATCHES that group consecutive
// triangles by their bound GX texture (texmap 0) — so the renderer can decode +
// bind each texture and draw its vertex range (N5: materials/textures).
#include <cstdint>

// One renderer-ready vertex: clip-space position, vertex color0, tex0 UV.
struct NgxRenderVertex {
    float clip[4];   // P·modelview·model (homogeneous clip space)
    float rgba[4];   // vertex color0, 0..1
    float uv[2];     // tex0 coord (S,T)
};

// A run of vertices sharing one bound GX texture (texmap 0). tex_addr==0 means
// "no/unsupported texture" → render flat (vertex color only). w/h/fmt are the
// GXTexObj fields (fmt = GX/SbTexFormat code) for the native decoder.
struct NgxRenderBatch {
    uint32_t tex_addr;        // guest address of the tiled texture bytes (0 = none)
    uint16_t w, h;            // texture dimensions
    uint8_t  fmt;             // GX texture format (SbTexFormat)
    uint32_t vstart, vcount;  // [vstart, vstart+vcount) into the vertex list
};

// Snapshot accessors (defined in ngx_j3d_shape.cpp; read best-effort from the
// HTTP/render thread — copy promptly, see ngx_j3d_shape.cpp notes).
const NgxRenderVertex* ngx_snap_verts(int* nverts);
const NgxRenderBatch*  ngx_snap_batches(int* nbatches);
