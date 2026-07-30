#pragma once
// native_render — the recomp's own GX renderer on SDL3 GPU (see native_render.cpp).
// Milestone 1: clip-space triangles. Grows toward a full GX pipeline, aurora as the oracle.

#include <cstdint>

// One vertex as the backend consumes it: position already in CLIP space (the frontend applies the
// position matrix and the projection on the CPU — the same contract the retired Path-B renderer's
// NvkTevVertex used), plus a colour so a draw can be identified without a pipeline change.
struct SbrVertex {
    float x, y, z, w;
    float r, g, b, a;
    // Four FINAL texture coordinates, one per texgen — the texgen (source select + texture matrix)
    // is evaluated on the CPU alongside the lighting, because its matrices animate per frame and
    // its source can be the position or normal rather than a stored coordinate.
    float uv[4][2];
    // Rasterised colour channel 1, at the END. The vertex attribute offsets in native_render.cpp
    // are absolute byte offsets into this struct, so a field inserted in the MIDDLE silently
    // repoints every attribute after it: putting this after `a` made the pipeline read uv01 from
    // offset 32 — which was now channel 1 — and every texture coordinate shifted. Geometry was
    // byte-identical and the frame merely looked washed, which is why it survived four rounds of
    // attribution. Append here, or change the offsets; never one without the other.
    float r1, g1, b1, a1;
};

// The GX depth state a draw is issued under, mirrored from GXSetZMode (overrides/gx_state_capture).
// `func` is a raw GXCompare (0=NEVER, 1=LESS, 2=EQUAL, 3=LEQUAL, 4=GREATER, 5=NEQUAL, 6=GEQUAL,
// 7=ALWAYS) — kept in GX terms so the translation to the backend lives in exactly one place.
struct SbrDepthState {
    uint8_t test;    // depth test enabled
    uint8_t func;    // GXCompare
    uint8_t write;   // depth WRITE enabled — the bit the sky relies on being off
    // Blend state, from GXSetBlendMode. A translucent full-screen overlay drawn with the depth test
    // DISABLED paints over the whole scene when blending is ignored, which is indistinguishable
    // from a depth bug until the blend state is actually honoured.
    uint8_t blend;   // GXBlendMode: 0=NONE, 1=BLEND, 2=LOGIC, 3=SUBTRACT
    uint8_t srcFac;  // GXBlendFactor
    uint8_t dstFac;  // GXBlendFactor
    // cmode0 bits 3 and 4. A draw with colorUpdate CLEAR writes no colour at all — the hardware
    // runs the whole pipeline and discards the result. Ignoring it turns such a draw into an
    // opaque black fill, which is indistinguishable from a shading bug.
    uint8_t colorUpdate;
    uint8_t alphaUpdate;
    // GENMODE bits 14-15, with GX's front/back convention already swapped (see the parser). 0 NONE,
    // 1 FRONT, 2 BACK, 3 ALL. Never implemented before: the pipeline hardcoded CULLMODE_NONE, so
    // this port rasterised the inside of every closed model — measured, aurora and this port
    // disagreed on cull for 27409 of 29497 draws (93%).
    uint8_t cull;
    // The per-draw SCISSOR rect (BP 0x20/0x21), in EFB pixels. The hardware confines every draw to
    // it; this port rasterised over the whole target, so a quad the hardware clips to a small band
    // was painted across the frame. Not pipeline state — applied with SDL_SetGPUScissor per batch —
    // but it IS part of a batch's identity, or draws with different clips merge into one.
    int16_t scissor[4];
};

// The texture bound to TEXMAP0, as the game described it to the hardware.
struct SbrTexture {
    uint32_t addr = 0;     // guest address of the tiled texture data
    uint32_t tlut = 0;     // palette address for the colour-indexed formats
    uint32_t width = 0, height = 0;
    uint32_t format = 0;   // GXTexFmt
    // TX_SETMODE0: how the hardware samples OUTSIDE [0,1] and between texels. A repeated ground
    // texture sampled with CLAMP smears its edge row across the whole surface, and a clamped UI
    // texture sampled with REPEAT wraps its border in — the two are not interchangeable defaults.
    uint8_t wrapS = 1, wrapT = 1;   // GXTexWrapMode: 0 CLAMP, 1 REPEAT, 2 MIRROR
    uint8_t magLinear = 1;          // GX_NEAR / GX_LINEAR
    uint8_t minLinear = 1;
    // A global bind counter, stamped when this unit was last written. Comparing a drawable's
    // stamps across units answers the question that texture VARIETY cannot: a unit with few
    // distinct images may be a shared light map (correct) or a unit nobody rebinds (stale), and
    // those look identical in a distinct-address count. The stamp distinguishes them.
    uint32_t bindSeq = 0;
};
SbrTexture sbr_gx_current_texture();

// The texture bound to `texmap` according to the GX command stream — the source that sees the
// per-material display lists J3D actually binds through.
SbrTexture sbr_gx_fifo_texture(unsigned texmap);

// One TEV stage, in GX's own encoding. The shader evaluates these directly rather than a
// pre-baked approximation, so the selector values keep their hardware meaning.
struct SbrTevStage {
    uint8_t texmap = 0, texcoord = 0, texEnable = 0, rasChannel = 0;
    uint8_t cA = 0, cB = 0, cC = 0, cD = 0;      // GXTevColorArg
    uint8_t cBias = 0, cSub = 0, cClamp = 1, cScale = 0, cDest = 0;
    uint8_t aA = 0, aB = 0, aC = 0, aD = 0;      // GXTevAlphaArg
    uint8_t aBias = 0, aSub = 0, aClamp = 1, aScale = 0, aDest = 0;
    uint8_t kC = 0, kA = 0;                       // konst selectors
    // TEV swap-mode selectors (TEV_ALPHA_ENV bits 0..1 / 2..3): which SbrTevState::swapTable row
    // remaps the rasterised colour and the texel before this stage reads them. This is how a
    // material reads a channel's ALPHA as an RGB grey — e.g. the plaza terrain's last stage reads
    // RASC through a swap row of A,A,A,A, so ignoring swap turned a white 1.0 into black.
    uint8_t swapRas = 0, swapTex = 0;
};

struct SbrTevState {
    uint32_t numStages = 1;
    uint32_t numTexGens = 1;
    SbrTevStage stage[16];
    float reg[4][4]{};       // TEV colour registers: prev, c0, c1, c2
    float konstReg[4][4]{};  // KONST registers k0..k3 — a separate bank, not the same storage
    // TEV_ALPHAFUNC (BP 0xF3). The cutout mechanism: foliage, fences and grates are drawn as opaque
    // quads whose shape comes ENTIRELY from discarding texels that fail this test. Without it those
    // surfaces render as solid rectangles, which reads as a geometry defect rather than a missing
    // pixel-pipeline stage.
    uint8_t alphaOp0 = 7, alphaOp1 = 7;   // GXCompare, 7 = ALWAYS (the power-on state)
    uint8_t alphaRef0 = 0, alphaRef1 = 0;
    uint8_t alphaLogic = 0;               // 0 AND, 1 OR, 2 XOR, 3 XNOR
    // The four TEV swap tables (TEV_KSEL bits 0..3, two components per register — the same
    // registers that carry the konst selectors). swapTable[t][outComp] = source channel (0 R, 1 G,
    // 2 B, 3 A): output component c reads source channel swapTable[t][c]. Power-on rows are
    // identity; GXInit's conventional rows 1..3 (RRRA/GGGA/BBBA) arrive through the stream.
    uint8_t swapTable[4][4] = {{0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3}};
    // When each RAS1_TREF register (0x28+r, two stages each) was last written, on the same clock
    // as SbrTexture::bindSeq. stage[] is global and persistent, so if numStages exceeds what the
    // material actually set, the loop reads per-stage texmap fields left by an EARLIER material.
    // These stamps are how that is detected rather than assumed.
    uint32_t trefSeq[8]{};
};

const SbrTevState& sbr_gx_fifo_tev();

// Byte offset the FIFO parser has reached in the current frame's stream.
uint32_t sbr_gxfifo_stream_pos();

// Tag the draws that follow with a stable cross-tick identity (aurora's GX_AURORA_DRAW_TAG), for
// interpolated 60fps. Drains pending guest FIFO bytes first so the tag lands at the right point in
// the stream. 0 clears the tag.
void sbr_gxfifo_draw_tag(uint64_t tag);

// Hand aurora this tick's J3D view matrix, for camera interpolation of draws that cannot be paired.
// Call once per tick, at the END of the tick's GX stream.
void sbr_gxfifo_view_matrix();

// GX lighting, as XF memory holds it. Lights are in VIEW space, which is the space the draw
// matrix already puts vertices into.
struct SbrLight {
    float pos[3]{}, dir[3]{};
    float color[4]{1, 1, 1, 1};
    float cosAtt[3]{1, 0, 0};
    float distAtt[3]{1, 0, 0};
};

struct SbrChanCtrl {
    uint8_t matSrcVertex = 1;   // material colour from the vertex rather than the register
    uint8_t enableLight = 0;
    uint8_t lightMask = 0;      // which of the 8 lights contribute
    uint8_t ambSrcVertex = 0;
    uint8_t diffuseFn = 0;      // 0 none, 1 sign, 2 clamp
    uint8_t attnEnable = 0;
    uint8_t attnSpot = 0;       // 0 = specular, 1 = spotlight
};

// One texture-coordinate generator. GX does NOT hand the vertex's raw TEX0 to the sampler: every
// coordinate is produced by a texgen, which picks a SOURCE (the vertex's texcoord, but equally its
// position or normal — that is how environment mapping is expressed) and runs it through a texture
// matrix. Taking the raw TEX0 is only correct for the identity case; scrolling water, the shiny
// reflective materials and anything with an animated texture SRT are all texgen configurations.
struct SbrTexGen {
    uint8_t type = 0;        // 0 REGULAR, 1 EMBOSS, 2 COLOR0, 3 COLOR1
    uint8_t sourceRow = 5;   // 0 GEOM, 1 NORMAL, 2 COLORS, 3/4 BINORMAL, 5 TEX0 .. 12 TEX7
    uint8_t projection = 0;  // 0 = ST (2x4), 1 = STQ (3x4, perspective divide by q)
    uint8_t inputForm = 0;   // 0 = (a,b,1,1), 1 = (a,b,c,1)
    uint8_t mtxSlot = 0xFF;  // texture-matrix slot 0..9, or 0xFF for GX_IDENTITY
};

struct SbrXfState {
    SbrLight light[8];
    SbrChanCtrl chan[4];        // colour0, colour1, alpha0, alpha1
    float ambient[2][4]{};
    float material[2][4]{};
    uint32_t numChans = 1;
    SbrTexGen texGen[8];
    // The ten GX texture matrices, row-major 3x4 as XF memory holds them.
    float texMtx[10][12]{};
    // Which of those slots the game has actually written. GX also loads texture matrices by INDEX
    // (from the array GXSetArray names), a path this parser does not follow — so a texgen can
    // select a slot that is still all zeroes, which would collapse every coordinate to a single
    // texel. That is a decode gap and has to announce itself rather than draw a plausible-looking
    // wrong surface.
    uint32_t texMtxWritten = 0;
};

const SbrXfState& sbr_gx_fifo_xf();

// The state the game has currently set. Valid at J3DShape::draw time.
SbrDepthState sbr_gx_current_zmode();
// The same state as the FIFO describes it (BP 0x40/0x41) rather than as the SDK overrides saw it.
SbrDepthState sbr_gx_fifo_zmode();
// Resolve GX's konst selector to a colour. Shared by the capture and the uniform packer so the two
// cannot disagree about what a stage's KONST means.
void sbr_tev_konst(const SbrTevState& tev, unsigned stage, float out[4]);

// The projection matrix currently loaded (4x4 row-major, as the guest built it) and whether it is a
// 2D ortho. Valid at J3DShape::draw time.
void sbr_gx_set_projection(const float m[16], bool is2d);
const float* sbr_gx_current_projection(bool* is2d);

// SBR_SDLGPU=1 selects the native path (off by default during bring-up).
bool sbr_render_enabled();

// Stand up the SDL3 GPU device + an EFB-sized offscreen colour/depth target. Idempotent; false if
// no device could be created (the caller keeps using aurora).
bool sbr_render_init(int w, int h);

// One frame: begin (records the clear colour and drops last frame's geometry), submit triangles as
// often as needed, then end (uploads, renders in one pass, downloads for readback).
void sbr_render_begin(float r, float g, float b, float a);
// Submit triangles under a given depth state. Consecutive submissions sharing a state are merged
// into one draw; a change of state starts a new one.
void sbr_render_tris(const SbrVertex* verts, int count, SbrDepthState depth, const SbrTexture tex[8],
                     const SbrTevState& tev);

// Per-BP-register write counts, so the per-unit texture BIND RATE is measurable rather than
// inferred (TX_SETIMAGE3 is 0x94+m for units 0-3).
void sbr_gxfifo_report_bp_writes();

// Re-decode every texture that cached black and report whether its guest bytes have since changed.
void sbr_render_recheck_black();

// How many distinct textures the renderer has decoded and uploaded.
int sbr_render_texture_count();

// One-shot histogram of the texture formats actually decoded.
void sbr_render_report_formats();

// How many separate draws the last frame needed (one per depth-state run) — the cost of honouring
// per-material state, and a signal that state is varying at all.
int sbr_render_last_batch_count();
void sbr_render_end();

// How many vertices the last completed frame drew — the cheapest "is the frontend producing
// geometry at all" signal.
int sbr_render_last_vertex_count();

// Write the last rendered frame to `path` (RGBA8, top-left origin) for the A/B against aurora.
bool sbr_render_dump(const char* path);

// Copy the last rendered frame into rgba (w*h*4, top-left origin). False on size mismatch / no device.
bool sbr_render_readback(uint8_t* rgba, int w, int h);

// ---- Operation attribution (see render_compare.h) ----
// How many ablations exist, what each is called, and re-rendering the current frame with one of
// them applied. The result replaces g_cpu, so sbr_render_readback returns the variant.
int sbr_render_ablation_count();
const char* sbr_render_ablation_name(int id);
bool sbr_render_ablation_render(int id);

// Bisect the batch list to find which batch first paints the given pixel black.
void sbr_render_report_black_owner(int px, int py);

// ---- EFB copy -> texture (render to texture) ----
// GX resolves a region of the EFB into a texture in main memory. This port never writes that
// memory back, so a draw binding a copy destination decoded ZEROS and multiplied the scene by
// black — that is what painted the Delfino background black (a 6-vertex full-screen quad sampling
// the 320x224 copy dest). Instead of faking the write-back, the copy is kept on the GPU: the
// region is blitted out of the render target into a texture registered under the destination
// address, and a bind of that address resolves to it. Same shape aurora uses.
//
// Recorded at the point in the DRAW ORDER where the copy happened, because this renderer is
// deferred: it batches a whole frame and draws at the end, while a copy must capture only what was
// drawn before it.
void sbr_render_note_copy(uint32_t dest, int sx, int sy, int sw, int sh, int dw, int dh);

// True if this guest address resolves to an EFB-copy surface rather than to decoded guest memory.
// The CPU TEV reference decodes guest memory, so for such an address it does NOT mirror what the
// GPU samples — it must say so rather than print a confident zero.
bool sbr_render_is_copy_surface(uint32_t addr);
// Read an EFB-copy surface back to a raw RGBA file, reporting its mean alpha.
void sbr_render_dump_copy(uint32_t addr, const char* path);
long sbr_gxfifo_take_draw_count();
void sbr_gxfifo_note_capture();
void sbr_gxfifo_take_uncaptured(long* trailing, long* longestRun);
