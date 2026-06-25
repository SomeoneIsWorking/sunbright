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

// Read the offscreen target back into `rgba` (w*h*4 bytes, top-left origin). Returns false if the
// target isn't the requested size or no context. Used by the present/dump path.
bool readback(uint8_t* rgba, int w, int h);

} // namespace sb::gxray
