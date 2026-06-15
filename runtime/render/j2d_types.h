#pragma once
// Shared between the J2D walker (j2d_walk.cpp, produces draw data from the live
// pane tree) and the native J2D renderer (j2d_render.cpp, draws it).
#include <cstdint>

// One renderable HUD quad: a J2DPicture's screen rect + its (raw, tiled) GC
// texture. Pixel rect in J2D screen space (y down, 0 at top). All fields are read
// from guest memory, bounds-checked at collection time.
struct J2dQuad {
    int      x0, y0, x1, y1;   // screen-space pixel rect (J2D global bounds)
    uint8_t  alpha;            // pane alpha (0..255)
    int      fmt;              // GC texture format (SbTexFormat)
    int      w, h;            // texture dimensions
    uint32_t data;             // guest addr of raw tiled image bytes (JUTTexture.mTexData)
    uint32_t tlut;             // guest addr of palette entries (paletted formats), else 0
    int      tlutfmt;          // SbTlutFormat (paletted only)
    uint32_t corner[4];        // J2DPicture mCornerColor[TL,TR,BL,BR], packed 0xRRGGBBAA (RASC)
};

// Walk the live J2D pane tree (root captured by the J2DScreen::draw tee) and fill
// `out` with visible J2DPicture quads, in draw order (parent before child, first
// child first). Returns the count (<= max). Also returns the root screen size.
int sb_j2d_collect(J2dQuad* out, int max, int* screen_w, int* screen_h);

// Read the latest CONSISTENT HUD draw list, captured on the game thread right after
// J2DScreen::draw (so mGlobalBounds are fully computed). Use this from the render/
// video thread instead of sb_j2d_collect to avoid the mid-update read race.
int sb_j2d_snapshot(J2dQuad* out, int max, int* screen_w, int* screen_h);
