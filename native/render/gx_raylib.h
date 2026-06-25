// gx_raylib.h — raylib/rlgl backend for the GX seam (the GX→raylib switch).
//
// Per the user directive (2026-06-25), the GameCube GX API is being reimplemented to translate
// directly into raylib/rlgl calls instead of the bespoke capture→Vulkan(nvk) rasterizer. This
// module owns the headless GL context + offscreen render target and exposes the primitives the
// GX seam routes onto (P1: context + clear + readback; later phases add immediate-mode geometry,
// matrices, textures, and TEV shaders). See docs/gx_raylib_switch.md.
//
// Headless: a hidden GLFW window on DISPLAY=:0 gives a working offscreen GL context (proven in
// scratch/raylib_smoke). All raylib calls MUST happen on ONE thread (the engine draw/present
// thread); the context is created lazily on first use from that thread.
#pragma once
#include <cstdint>
#include "nvk.h"   // NvkTevVertex / Nvk::NvkTevBatch (the captured scene + 2D imm geometry)

namespace sb::gxray {

// SB_RAYLIB=1 selects the raylib backend (default off during bring-up). Cached on first read.
bool enabled();

// Lazily create the GL context + an offscreen render target of (w,h). Idempotent; returns false
// if no GL context could be created (then the caller falls back to the nvk path).
bool init(int w, int h);

// Begin/end a frame into the offscreen target. frame_begin binds the target and clears it to the
// given GX copy-clear colour (0..1). Draws (P2+) happen between these.
void frame_begin(float r, float g, float b, float a);
void frame_end();

// P3: replay the captured combined geometry (scene 3D + 2D imm batches, the same `verts`/
// `batches` the nvk path consumes) into the bound offscreen target through a custom rlgl vertex
// buffer + GLSL-330 program. Must be called between frame_begin and frame_end. The verts are
// Vulkan clip-space (y-down, depth [0,1]); this uploads them as a 4-COMPONENT clip-space position
// (converted to GL clip-space: x, -y, 2z-w, w) so the GPU does native near/side clipping +
// perspective-correct interpolation — exactly nvk's contract (replacing P2's CPU-approximate
// near-clip, which left a sky starburst + black foreground disc). FS = texture0 * vertexColor
// (GX_MODULATE), per-batch texture/blend/depth. (Per-material TEV combiner shaders are P4.)
void draw_tev(const sb::render::NvkTevVertex* verts, int nverts,
              const sb::render::Nvk::NvkTevBatch* batches, int nbatch);

// Read the offscreen target back into `rgba` (w*h*4 bytes, top-left origin). Returns false if the
// target isn't the requested size or no context. Used by the present/dump path.
bool readback(uint8_t* rgba, int w, int h);

} // namespace sb::gxray
