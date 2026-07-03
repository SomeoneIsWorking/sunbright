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
// (Single render pass that CLEARS the target — equivalent to draw_tev_segment(..., true).)
void draw_tev(const sb::render::NvkTevVertex* verts, int nverts,
              const sb::render::NvkTevBatch* batches, int nbatch);

// Render ONE segment of the frame between EFB-copy boundaries (the GC multi-EFB-target structure;
// see the 2026-06-30 file-select overbright journal). Call draw_tev_segment / snapshot_efb in
// sequence between frame_begin and frame_end:
//   - clearFirst=true  → the render pass CLEARS colour+depth (a fresh EFB, e.g. after a clearing copy)
//   - clearFirst=false → the render pass LOADS (preserves) the prior segment's colour+depth
// `verts`/`nverts` is always the FULL combined vertex list (batch vstart indices are absolute); only
// the `batches` sub-range is rendered. A batch slot carrying Tex::efb_src samples the snapshot
// registered under that key (below) instead of its decoded `rgba`.
void draw_tev_segment(const sb::render::NvkTevVertex* verts, int nverts,
                      const sb::render::NvkTevBatch* batches, int nbatch, bool clearFirst);

// Snapshot the current offscreen colour into a GPU texture registered under `key` (the EFB-copy
// destination pointer). A later batch whose Tex::efb_src == key samples this snapshot. Snapshots are
// released at the next frame_begin. Must be called between segments (not inside a render pass).
void snapshot_efb(const void* key);

// SMS_NATIVE_PLATFORM sky pass (CLAUDE.md 2026-07-03 hard rule). Paints a vertical blue gradient
// full-screen — the visual intent of TSky's backdrop-sphere + sky.bmd dome, rendered as a native
// SDL3-GPU pass instead of trying to make the game's off-screen sky.bmd batches project correctly
// (see debug_journal/2026-07-03_sky_bmd_offscreen.md). `top` and `horizon` are RGBA[0..1] endpoints
// (top = sky zenith, horizon = ground line). Called BETWEEN frame_begin and the first
// draw_tev_segment, using LOAD_OP=LOAD so it paints over the clear without erasing depth.
void native_sky_fill(const float top[4], const float horizon[4]);

// SMS_NATIVE_PLATFORM zzz sleep bubbles pass (CLAUDE.md 2026-07-03 hard rule). Paints up to N
// screen-space quads with soft "Z" glyphs, blended over the composited scene — the visual intent
// of the JPA `PARTICLE_MS_POI_ZZZ` particle above sleeping Mario's head, native-owned instead of
// chasing the JPA NaN dispatch chain (see debug_journal/2026-07-03_zzz_particle_nan_diagnosis.md).
// Each quad = (ndc_x, ndc_y, half_size_ndc, alpha01). Call AFTER all draw_tev_segment calls,
// BEFORE frame_end. Blend = SRC_ALPHA/INV_SRC_ALPHA. Ignored when count<=0 or no frame active.
void native_zzz_paint(const float quads[][4], int count);

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
