// gx_sdlgpu.h — SDL3 GPU backend for the GX seam (the GX→SDL3-GPU switch).
//
// Per the user directive (2026-06-25), the GameCube GX API's PC-native renderer runs on the SDL3 GPU
// API (SDL_gpu.h), replacing the bespoke nvk Vulkan rasterizer. It is now the sole renderer.
// SDL3 GPU's Vulkan backend consumes SPIR-V, so the existing TEV shader pipeline
// (sb_tev_gen_fragment + glslang + tev_vert_spv) plugs in, and its NDC matches Vulkan so nvk's
// clip-space NvkTevVertex feeds as-is. See docs/gx_sdlgpu_switch.md.
//
// Headless: SDL_Init(SDL_INIT_VIDEO) is required even windowless; SDL_VIDEODRIVER=offscreen gives a
// no-DISPLAY GPU device (proven in scratch/sdlgpu_smoke). All calls happen on the present thread.
#pragma once
#include <cstdint>
#include "gx_geom.h"   // NvkTevVertex / NvkTevBatch (the captured scene + 2D imm geometry contract)

namespace sb::gxsdl {

// SB_SDLGPU=1 selects the SDL3 GPU backend (default off during bring-up). Cached on first read.
bool enabled();

// Lazily create the GPU device + offscreen color (and depth) targets of (w,h). Idempotent; returns
// false if no GPU device could be created (the caller then falls back to nvk).
bool init(int w, int h);

// Begin/end a frame into the offscreen target. frame_begin clears to the GX copy-clear colour
// (0..1). draw_tev (P2+) records geometry between them; frame_end resolves + downloads to a CPU
// buffer that readback() reads.
void frame_begin(float r, float g, float b, float a);
void frame_end();

// P2+: render the captured combined geometry (scene 3D + 2D imm batches — the same verts/batches the
// nvk path consumes) into the bound offscreen target. Must be called between frame_begin/frame_end.
void draw_tev(const sb::render::NvkTevVertex* verts, int nverts,
              const sb::render::NvkTevBatch* batches, int nbatch);

// Read the last frame back into rgba (w*h*4, top-left origin). False if size mismatch / no device.
bool readback(uint8_t* rgba, int w, int h);

// Live window (SB_WINDOW=1) — the SDL MAIN-thread present path (see gx_sdlgpu.cpp). The game runs on
// a separate thread and renders into g_cpu; the SDL main thread calls present_window() in a loop to
// show it. present_window() MUST be called only from the SDL main thread (never the game thread).
//   window_mode()      — is SB_WINDOW set?
//   window_preinit()   — SDL_Init(VIDEO) on the main thread, before the game thread starts.
//   present_window()   — pump events + blit g_cpu to the window (main thread).
//   window_should_quit / game_done / mark_game_done — main-loop exit conditions.
bool window_mode();
void window_preinit();
void present_window();
bool window_should_quit();
void mark_game_done();
bool game_done();

} // namespace sb::gxsdl
