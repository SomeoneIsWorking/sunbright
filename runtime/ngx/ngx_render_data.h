#pragma once
// ngx_render_data — the snapshot contract between the J3DShape capture
// (ngx_j3d_shape.cpp, producer) and the native Vulkan renderer (vk_mesh.cpp,
// consumer). The capture records, per frame's-worth of shapes, a flat list of
// clip-space vertices plus a list of draw BATCHES that group consecutive
// triangles by their bound GX texture (texmap 0) — so the renderer can decode +
// bind each texture and draw its vertex range (N5: materials/textures).
#include <cstdint>

// One renderer-ready vertex: clip-space position, lit raster color0, and the 8 GX
// texcoord UVs (one per GX_TEXCOORD0..7) with the per-material GX texgen already
// applied on the CPU (N6.6). A TEV stage samples uv[its texcoord].
struct NgxRenderVertex {
    float clip[4];    // P·modelview·model (homogeneous clip space)
    float rgba[4];    // lit raster color0 (N6 native lighting; = CLR0 when unlit), 0..1
    float uv[8][2];   // texgen'd UV per GX texcoord 0..7
};

// One bound GX texture (a single texmap). addr==0 means "no/unsupported texture"
// → sampled as 1×1 white. w/h/fmt/tlut* are the GXTexObj fields the native decoder
// (tex_decode) needs (fmt = GX/SbTexFormat code).
struct NgxTexBind {
    uint32_t addr;            // guest address of the tiled texture bytes (0 = none)
    uint16_t w, h;            // texture dimensions
    uint8_t  fmt;             // GX texture format (SbTexFormat)
    uint8_t  tlut_fmt;        // GX TLUT format (SbTlutFormat) for CI formats
    uint32_t tlut_addr;       // guest palette address for CI formats (0 = none)
};

// A run of vertices sharing one material AND the same set of bound texmaps. The
// TEV stages select which texmap each samples (tex[GX_TEXMAP0..7]); a batch breaks
// when the material OR any texmap binding changes.
struct NgxRenderBatch {
    NgxTexBind tex[8];        // bound texture per GX texmap (0..7)
    uint32_t vstart, vcount;  // [vstart, vstart+vcount) into the vertex list
    int32_t  tev_index;       // index into the TEV-state table (-1 = none/default)
};

// ── N5 per-material TEV state (read from the guest J3DMaterial's mTevBlock) ─────
// One TEV stage = the two GX combiner BP register values plus its order/konst
// selection. The J3DTevStage 8 bytes ARE the two 24-bit GX combiner registers
// (color_env = TevStageCombiner::ColorCombiner, alpha_env = AlphaCombiner), so we
// store them raw and decode the bitfields in the shader generator (tev_shader).
struct NgxTevStage {
    uint32_t color_env;   // 24-bit GX color combiner reg (d/c/b/a, bias, op, clamp, scale, dest)
    uint32_t alpha_env;   // 24-bit GX alpha combiner reg (rswap, tswap, d/c/b/a, bias, op, clamp, scale, dest)
    uint8_t  texcoord;    // GX_TEXCOORD0.. (0xff = null)
    uint8_t  texmap;      // GX_TEXMAP0..   (0xff = null)
    uint8_t  color_chan;  // GX raster color source (GX_COLOR0A0=0 .. GX_COLOR_NULL=0xff)
    uint8_t  kcsel;       // GX konst color selection (GXTevKColorSel)
    uint8_t  kasel;       // GX konst alpha selection (GXTevKAlphaSel)
    uint8_t  pad[3];
};

// ── N7 PE (pixel-engine) block: alpha test + blend + zmode (from mPEBlock) ──────
// The J3D PE block decides per-material framebuffer behaviour: an alpha test
// (compare the final TEV alpha against ref0/ref1, combined by an op) baked into
// the fragment shader as discard, plus blend + depth state set on the pipeline.
// GX enum values are kept raw (GXCompare/GXAlphaOp/GXBlendMode/GXBlendFactor) and
// translated by the shader generator (alpha test) / vk_mesh (blend, depth).
struct NgxPEState {
    uint8_t  alpha_test;      // 1 = emit discard (the compare is meaningful), 0 = always pass
    uint8_t  comp0, ref0;     // GXCompare comp0, u8 ref0
    uint8_t  aop;             // GXAlphaOp (0=AND 1=OR 2=XOR 3=XNOR)
    uint8_t  comp1, ref1;     // GXCompare comp1, u8 ref1
    uint8_t  blend_mode;      // GXBlendMode (0=NONE 1=BLEND 2=LOGIC 3=SUBTRACT)
    uint8_t  src_factor;      // GXBlendFactor (source)
    uint8_t  dst_factor;      // GXBlendFactor (dest)
    uint8_t  logic_op;        // GXLogicOp (unused by the GL/VK blend path)
    uint8_t  z_test;          // depth compareEnable
    uint8_t  z_func;          // GXCompare (== VkCompareOp values)
    uint8_t  z_write;         // depth updateEnable
    uint8_t  cull;            // GXCullMode (color block mCullMode): 0=NONE 1=FRONT 2=BACK 3=ALL
    uint8_t  pad[2];
};

// A whole material's TEV combiner state — the cache key for a generated shader.
struct NgxTevState {
    uint8_t  num_stages;       // 1..16
    uint8_t  pad[3];
    NgxTevStage stage[16];
    int16_t  tev_color[4][4];  // CPREV/C0/C1/C2 register init values, S10 RGBA (-1024..1023)
    uint8_t  kcolor[4][4];     // KONST0..3 RGBA, 0..255
    NgxPEState pe;             // N7 PE block (alpha test → shader, blend/zmode → pipeline)
    uint64_t key;              // FNV hash of the above (dedupe / shader-cache key)
};

// Snapshot accessors (defined in ngx_j3d_shape.cpp; read best-effort from the
// HTTP/render thread — copy promptly, see ngx_j3d_shape.cpp notes).
const NgxRenderVertex* ngx_snap_verts(int* nverts);
const NgxRenderBatch*  ngx_snap_batches(int* nbatches);
const NgxTevState*     ngx_snap_tevstates(int* nstates);
