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

// SBR_SDLGPU=1 selects the native path (off by default during bring-up).
bool sbr_render_enabled();

// Stand up the SDL3 GPU device + an EFB-sized offscreen colour/depth target. Idempotent; false if
// no device could be created (the caller keeps using aurora).
bool sbr_render_init(int w, int h);

// One frame: begin (records the clear colour and drops last frame's geometry), submit triangles as
// often as needed, then end (uploads, renders in one pass, downloads for readback).
void sbr_render_begin(float r, float g, float b, float a);
void sbr_render_tris(const SbrVertex* verts, int count);   // count must be a multiple of 3
void sbr_render_end();

// How many vertices the last completed frame drew — the cheapest "is the frontend producing
// geometry at all" signal.
int sbr_render_last_vertex_count();

// Copy the last rendered frame into rgba (w*h*4, top-left origin). False on size mismatch / no device.
bool sbr_render_readback(uint8_t* rgba, int w, int h);
