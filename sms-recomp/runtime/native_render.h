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
    float u, v;
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
};

// The texture bound to TEXMAP0, as the game described it to the hardware.
struct SbrTexture {
    uint32_t addr = 0;     // guest address of the tiled texture data
    uint32_t tlut = 0;     // palette address for the colour-indexed formats
    uint32_t width = 0, height = 0;
    uint32_t format = 0;   // GXTexFmt
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
};

struct SbrTevState {
    uint32_t numStages = 1;
    uint32_t numTexGens = 1;
    SbrTevStage stage[16];
    float reg[4][4]{};       // TEV colour registers: prev, c0, c1, c2
    float konstReg[4][4]{};  // KONST registers k0..k3 — a separate bank, not the same storage
};

const SbrTevState& sbr_gx_fifo_tev();

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

struct SbrXfState {
    SbrLight light[8];
    SbrChanCtrl chan[4];        // colour0, colour1, alpha0, alpha1
    float ambient[2][4]{};
    float material[2][4]{};
    uint32_t numChans = 1;
};

const SbrXfState& sbr_gx_fifo_xf();

// The state the game has currently set. Valid at J3DShape::draw time.
SbrDepthState sbr_gx_current_zmode();
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
void sbr_render_tris(const SbrVertex* verts, int count, SbrDepthState depth, SbrTexture tex,
                     const SbrTevState& tev);

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
