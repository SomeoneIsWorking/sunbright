#pragma once
// native_render — the recomp's own GX renderer on SDL3 GPU (see native_render.cpp).
// Milestone 0: device + clear + readback. Grows toward a full GX pipeline, aurora as the oracle.

#include <cstdint>

// SBR_SDLGPU=1 selects the native path (off by default during bring-up).
bool sbr_render_enabled();

// Stand up the SDL3 GPU device + an EFB-sized offscreen colour/depth target. Idempotent; false if
// no device could be created (the caller keeps using aurora).
bool sbr_render_init(int w, int h);

// Milestone 0: clear the offscreen target to (r,g,b,a) in 0..1 and download it for readback.
void sbr_render_clear(float r, float g, float b, float a);

// Copy the last rendered frame into rgba (w*h*4, top-left origin). False on size mismatch / no device.
bool sbr_render_readback(uint8_t* rgba, int w, int h);
