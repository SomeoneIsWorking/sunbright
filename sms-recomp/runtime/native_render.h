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

// The state the game has currently set. Valid at J3DShape::draw time.
SbrDepthState sbr_gx_current_zmode();

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
void sbr_render_tris(const SbrVertex* verts, int count, SbrDepthState depth);

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
