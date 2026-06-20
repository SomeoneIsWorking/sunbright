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
    float rgba1[4];   // lit raster COLOR1A1 (2nd GX colour channel; = color0 when the material
                      // has no distinct channel 1). A TEV stage with rasChan∈{COLOR1,ALPHA1}
                      // reads this; appended at the END so uv[] offsets are unchanged.
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
    // GX sampler state (from ResTIMG / GXTexObj). The present must honour these per
    // texture — a global REPEAT+LINEAR sampler washes a CLAMP/NEAR-sampled noise.
    uint8_t  wrap_s;          // GX wrap mode (0=CLAMP 1=REPEAT 2=MIRROR)
    uint8_t  wrap_t;
    uint8_t  min_filter;      // GXTexFilter (0=NEAR 1=LINEAR 2..6=mip variants)
    uint8_t  mag_filter;      // GXTexFilter (0=NEAR 1=LINEAR)
    uint8_t  mipmap;          // mipmapEnabled (0/1)
    uint8_t  mip_count;       // ResTIMG mipmapCount (number of stored mip levels; >=1). The
                              // floor texture's authored high mips are dark — sampling only
                              // level 0 renders minified tiled surfaces ~2x too bright.
    uint8_t  pad_s[2];
};

// A run of vertices sharing one material AND the same set of bound texmaps. The
// TEV stages select which texmap each samples (tex[GX_TEXMAP0..7]); a batch breaks
// when the material OR any texmap binding changes.
struct NgxRenderBatch {
    NgxTexBind tex[8];        // bound texture per GX texmap (0..7)
    uint32_t vstart, vcount;  // [vstart, vstart+vcount) into the vertex list
    int32_t  tev_index;       // index into the TEV-state table (-1 = none/default)
    uint16_t epoch;           // EFB-copy epoch this batch drew under (render-target awareness)
    uint16_t gen;             // clear-aware EFB generation this batch drew under (++ after a CLEARING
                              // copy). The present shows only the display generation (= the final
                              // gen, what GX's last CopyDisp captured) — GPU truth: the EFB accumulates
                              // across non-clearing copies and is reset ONLY by a clear, so a later
                              // small GXCopyTex must NOT demote the main scene (the Sirena-beach black
                              // bug the old highest-tex-closed-epoch heuristic caused).
    uint16_t pass;            // projection-pass index this batch drew under (offscreen-pass filtering)
    uint16_t vp_w, vp_h;      // GXSetViewport extent this batch drew under (px). A sub-display
                              // viewport = an offscreen render-to-texture pass (file-panel preview,
                              // mirror, pollution) ngx cannot composite — the present drops it.
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
    // ── framebuffer write masks (GXSetColorUpdate / GXSetAlphaUpdate / GXSetDstAlpha) ─────
    // Default 0 ⇒ write RGB+A, no forced dst-alpha (every J3D batch behaves as before — capture_pe
    // leaves these zero). Set ONLY by the immediate-mode GXDrawCube capture (the Mario occlusion
    // probe writes a constant dst-alpha 0x10 with colour writes masked OFF). The pipeline turns
    // these into the Vulkan colorWriteMask; the const dst-alpha is fed via the vertex alpha
    // (PASSCLR) so no shader-generator change is needed.
    uint8_t  color_mask_off;  // 1 = do NOT write RGB (GXSetColorUpdate(FALSE))
    uint8_t  alpha_mask_off;  // 1 = do NOT write A   (GXSetAlphaUpdate(FALSE))
};

// ── Indirect texturing (mIndBlock + mIndTevStage) ───────────────────────────────
// GX indirect texturing warps a regular TEV stage's texcoord by an offset sampled
// from an "indirect" texture. Faithful to GX hardware (mirrors Dolphin's
// PixelShaderGen / GXBump.c GXSetIndTexMtx): all coords are fixpoint = uv*texsize*128.
// See debug_journal/2026-06-19_indirect_texturing_re.md for the full derivation.
struct NgxIndTevStage {       // per regular-TEV-stage indirect config (J3DIndTevStage)
    uint8_t enabled;          // 1 = this stage applies an indirect warp (has stage AND mtx_sel != OFF)
    uint8_t ind_stage;        // mIndStage  (which indirect stage's lookup feeds this TEV stage)
    uint8_t format;           // mIndFormat (GX_ITF_8/5/4/3 = 0..3)
    uint8_t bias_sel;         // mBiasSel   (GX_ITB_*  0..7: 0=NONE,1=S,2=T,3=ST,4=U,5=SU,6=TU,7=STU)
    uint8_t mtx_sel;          // mMtxSel    (GXIndTexMtxID: 0=OFF,1-3=ITM_0..2,5-7=S0..2,9-11=T0..2)
    uint8_t wrap_s, wrap_t;   // mWrapS/T   (GX_ITW_*  0=OFF,1=256,2=128,3=64,4=32,5=16,6=ITW_0)
    uint8_t add_prev;         // mPrev      (fb_addprev: add to previous tevcoord)
    uint8_t alpha_sel;        // mAlphaSel  (bump-alpha select; captured — applied only if simple)
    uint8_t pad_i[3];
};
struct NgxIndirect {
    uint8_t  num_stages;          // mIndTexStageNum (0 = material uses no indirect texturing)
    uint8_t  pad0[3];
    uint8_t  order_coord[4];      // per ind stage: J3DIndTexOrder.mCoord (GX_TEXCOORD)
    uint8_t  order_map[4];        // per ind stage: J3DIndTexOrder.mMap   (GX_TEXMAP)
    uint8_t  scale_s[4];          // per ind stage: IndTexCoordScale.mScaleS (GX_ITS_* = log2 shift)
    uint8_t  scale_t[4];          // per ind stage: IndTexCoordScale.mScaleT
    int16_t  mtx[3][2][3];        // 3 IndTexMtx: 11-bit signed mantissas (round(1024*mOffsetMtx))
    int8_t   mtx_exp[3];          // per matrix: mScaleExp (s8)
    uint8_t  pad1;
    NgxIndTevStage stage[16];     // per regular TEV stage
};

// A whole material's TEV combiner state — the cache key for a generated shader.
struct NgxTevState {
    uint8_t  num_stages;       // 1..16
    uint8_t  pad[3];
    NgxTevStage stage[16];
    int16_t  tev_color[4][4];  // CPREV/C0/C1/C2 register init values, S10 RGBA (-1024..1023)
    uint8_t  kcolor[4][4];     // KONST0..3 RGBA, 0..255
    // The 4 GX TEV swap tables (mTevSwapModeTable[4], J3DTevSwapModeTable::mIdx). Each byte
    // encodes a 4-channel swizzle: r=(idx>>6)&3, g=(idx>>4)&3, b=(idx>>2)&3, a=idx&3, with
    // selector 0=R 1=G 2=B 3=A. Per stage, rswap (alpha_env bits 0-1) picks the table applied
    // to the raster colour, tswap (bits 2-3) the table applied to the texture colour. Default
    // table id = 0x1B (identity). TVB1 has no tables → all identity.
    uint8_t  swap_table[4];
    NgxPEState pe;             // N7 PE block (alpha test → shader, blend/zmode → pipeline)
    NgxIndirect ind;           // indirect texturing (texcoord warp); ind.num_stages==0 ⇒ none
    uint64_t key;              // FNV hash of the above (dedupe / shader-cache key)
};

// Snapshot accessors (defined in ngx_j3d_shape.cpp; read best-effort from the
// HTTP/render thread — copy promptly, see ngx_j3d_shape.cpp notes).
const NgxRenderVertex* ngx_snap_verts(int* nverts);
const NgxRenderBatch*  ngx_snap_batches(int* nbatches);
const NgxTevState*     ngx_snap_tevstates(int* nstates);
int                    ngx_snap_display_epoch(void);   // present skips batches with epoch < this
int                    ngx_snap_display_gen(void);     // clear-aware: present shows only batches whose gen == this (the display generation; -1 = show all)

// ── interp60 (PC-native frame interpolation) ─────────────────────────────────
// The PREVIOUS published snapshot (frame N-1) kept alongside the front (N) so the present can
// render an in-between field: a clip-space lerp of N-1↔N (NgxRenderVertex.clip/rgba/uv), valid only
// when the two frames share topology (same vertex count + batch structure) — else the present falls
// back to N (interpolation can never break rendering). prev = nullptr until two frames have
// published. ngx_snap_verts_prev MUST be called before ngx_snap_batches_prev (it latches the buffer).
const NgxRenderVertex* ngx_snap_verts_prev(int* nverts);
const NgxRenderBatch*  ngx_snap_batches_prev(int* nbatches);
bool ngx_interp60_enabled();           // default ON under ngx capture; SUNBRIGHT_INTERP60=0 disables
int  ngx_interp_mode();                // 0 = render front (real frame), 1 = render the N-1↔N blend
extern "C" void  sb_ngx_set_interp_mode(int m);   // interp driver sets before each present
extern "C" float sb_ngx_interp_alpha();           // live blend alpha (default 0.5; /interp60 tunable)
extern "C" void  sb_ngx_set_interp_alpha(float a);
