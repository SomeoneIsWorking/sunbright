#pragma once
// j3d_decode — turn one J3DShape's guest-memory geometry into triangles.
//
// Spec: docs/re_notes/j3d_shape_decode.md (RE'd from decomp/sms). Everything here reads GUEST
// memory: big-endian, 32-bit pointers, no C++ casts available.

#include <cstdint>
#include <vector>

#include "intrinsics.h"

// The GX vertex attributes this decoder understands, in GXAttr enum order (which is also the order
// attributes appear in a vertex, so the enum doubles as the payload layout).
enum : uint32_t {
    GXA_PNMTXIDX = 0,
    GXA_TEX0MTXIDX = 1,   // .. 8 = TEX7MTXIDX
    GXA_POS = 9,
    GXA_NRM = 10,
    GXA_CLR0 = 11,
    GXA_CLR1 = 12,
    GXA_TEX0 = 13,        // .. 20 = TEX7
    GXA_NBT = 25,
    GXA_NULL = 0xFF,
    GXA_COUNT = 21,       // PNMTXIDX..TEX7 — the range that can appear in a vertex
};

enum : uint32_t { GXAT_NONE = 0, GXAT_DIRECT = 1, GXAT_INDEX8 = 2, GXAT_INDEX16 = 3 };

// The per-shape vertex layout: which attributes are present, how they are supplied, and their
// element formats. Built once per shape from mVtxDescList + mVtxAttrFmtList.
struct J3DVertexLayout {
    uint8_t  type[GXA_COUNT]{};    // GXAT_*
    uint32_t cnt[GXA_COUNT]{};     // GXCompCnt
    uint32_t comp[GXA_COUNT]{};    // GXCompType
    uint8_t  frac[GXA_COUNT]{};    // fixed-point shift
    uint32_t offset[GXA_COUNT]{};  // byte offset of this attribute within a vertex
    uint32_t vtxSize = 0;          // total bytes per vertex
    bool     nbt = false;          // normals carry N+B+T (stride x3, base = mVtxNBTArray)
    bool     valid = false;
};

// One decoded triangle vertex, model space, with the matrix slot it selected.
struct J3DVert {
    float x, y, z;
    uint32_t pnMtxSlot;   // PNMTXIDX/3 — which J3DShapeMtx slot this vertex uses
    float nx, ny, nz;     // NRM, model space — needed for the lit colour channel
    float u, v;           // TEX0
    uint32_t rgba;        // CLR0, 0xRRGGBBAA; opaque white when the shape has no colour attribute
};

// Build the layout for a shape. False if the shape's descriptor/format tables are unreadable.
bool j3d_build_layout(u32 shape, J3DVertexLayout& out);

// Decode one element's geometry display list into a triangle list (model-space positions).
// `element` indexes [shape+0x38]. Appends to `out`. Returns false on a malformed list — which is a
// desync, not a soft failure: a wrong vertex size silently renders garbage, so it must be loud.
bool j3d_decode_element(u32 shape, uint32_t element, const J3DVertexLayout& layout,
                        std::vector<J3DVert>& out);
