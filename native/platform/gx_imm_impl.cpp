// gx_imm_impl.cpp — native GX IMMEDIATE-MODE capture (SLICE 2 of renderer-attach).
//
// The GameCube immediate-mode draw API (GXBegin / GXPosition* / GXColor* / GXEnd)
// streams vertices into the write-gather FIFO. The native renderer has no FIFO and
// reads the J3D OBJECT MODEL for scene geometry — but 2D/HUD content (the fader, the
// GC-logo overlay, J2D windows, font glyphs) is drawn ONLY through this immediate API,
// so the object-model path misses it. We capture it here instead: GXVert.h routes the
// immediate writers to the sb_gx_imm_* hooks below (under SMS_NATIVE_PLATFORM), which
// build native vertices, transform them through the captured GXState projection +
// position matrix (gx_imm_xform.h, a pure unit-tested function), triangulate, and hand
// the resulting Vulkan-NDC triangle list to the present layer (sms_boot_present.cpp).
//
// This is NOT a FIFO emulator: it captures the game's drawn quads at the API seam and
// re-projects them, the sanctioned "rebuild as a PC game" path (see CLAUDE.md ground
// rules + memory ngx-imm-geometry-and-swaptable).
//
// Threading: the engine draws and presents on ONE thread (VIWaitForRetrace -> present
// fires synchronously at frame end, after the gameLoop's draws). So no locking: the
// frame triangle list accumulates during the frame and the present consumes it. We
// clear lazily on the first GXBegin AFTER a present consumed, so several presents within
// one frame (waitForRetrace loops) all render the same accumulated frame.

#include <dolphin/gx.h>      // GXColor etc. (gx_state.h uses them)
#include "gx_state.h"
#include "gx_imm_xform.h"
#include <cstdio>
#include <cstdlib>

using sb::platform::gx::state;
using sb::render::SbImmVtx;
using sb::render::SbImmRawVtx;

namespace {

// --- accumulated triangle list for the current frame (consumed by present) ---
std::vector<SbImmVtx> g_frame_tris;
bool g_consumed = true;   // start true so the first GXBegin clears the (empty) buffer

// --- in-progress primitive (between GXBegin and GXEnd) ---
bool  g_in_begin = false;
int   g_prim = 0;
std::vector<SbImmVtx> g_prim_verts;

// projection/position/viewport snapshot taken at GXBegin (state is set before the draw).
int   g_projType = 1;
float g_projMtx[6] = {0};
float g_posMtx[3][4] = {{0}};
float g_vp[6] = {0, 0, 640, 480, 0, 1};

// current immediate-mode colour (GXColor* sets it; applied to the next/last vertex).
float g_cr = 1, g_cg = 1, g_cb = 1, g_ca = 1;

// SB_GX_IMM_DBG=1: dump the first few captured verts (proj/viewport/NDC) once, to verify
// the transform against the live engine state. One-shot (g_dbg_left), then silent.
const bool g_dbg = [] { const char* e = std::getenv("SB_GX_IMM_DBG"); return e && e[0] && e[0] != '0'; }();
int g_dbg_left = 12;

void snapshot_state() {
    auto& g = state();
    g_projType = (int)g.projType;
    for (int i = 0; i < 6; ++i) g_projMtx[i] = g.projMtx[i];
    const u32 slot = g.currentMtx / 3;
    const auto& m = g.posMtx[slot < 64 ? slot : 0];
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) g_posMtx[r][c] = m[r][c];
    g_vp[0] = g.vpLeft; g_vp[1] = g.vpTop; g_vp[2] = g.vpWd; g_vp[3] = g.vpHt;
    g_vp[4] = g.vpNearz; g_vp[5] = g.vpFarz;
}

} // namespace

extern "C" {

// ---- capture hooks (called from GXVert.h immediate writers) ----------------
void sb_gx_imm_begin(int prim) {
    if (g_consumed) { g_frame_tris.clear(); g_consumed = false; }
    g_in_begin = true;
    g_prim = prim;
    g_prim_verts.clear();
    snapshot_state();
}

void sb_gx_imm_pos(float x, float y, float z) {
    if (!g_in_begin) return;
    SbImmRawVtx raw{ x, y, z, g_cr, g_cg, g_cb, g_ca };
    SbImmVtx p = sb::render::imm_project(raw, g_projType, g_projMtx, g_posMtx, g_vp);
    g_prim_verts.push_back(p);
    if (g_dbg && g_dbg_left > 0) {
        --g_dbg_left;
        std::fprintf(stderr, "[gx_imm] prim=%#x pos(%.1f,%.1f,%.1f) -> ndc(%.3f,%.3f,%.3f) "
                     "proj=%d pm[%.4f,%.4f,%.4f,%.4f] vp[%.0f,%.0f,%.0f,%.0f]\n",
                     g_prim, x, y, z, p.x, p.y, p.z, g_projType,
                     g_projMtx[0], g_projMtx[1], g_projMtx[2], g_projMtx[3],
                     g_vp[0], g_vp[1], g_vp[2], g_vp[3]);
    }
}

// GXColor* sets the colour for the vertex JUST submitted (GX order is pos then colour)
// and becomes the running colour for any subsequent position with no colour of its own.
void sb_gx_imm_color_rgba(unsigned r, unsigned g, unsigned b, unsigned a) {
    g_cr = r / 255.0f; g_cg = g / 255.0f; g_cb = b / 255.0f; g_ca = a / 255.0f;
    if (g_in_begin && !g_prim_verts.empty()) {
        auto& v = g_prim_verts.back();
        v.r = g_cr; v.g = g_cg; v.b = g_cb; v.a = g_ca;
    }
}
void sb_gx_imm_color_u32(unsigned c) {   // packed 0xRRGGBBAA
    sb_gx_imm_color_rgba((c >> 24) & 0xff, (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
}

void sb_gx_imm_end(void) {
    if (!g_in_begin) return;
    g_in_begin = false;
    sb::render::imm_triangulate(g_prim, g_prim_verts.data(),
                                (int)g_prim_verts.size(), g_frame_tris);
}

// ---- present bridge --------------------------------------------------------
// Hand the present layer the current frame's triangle list (Vulkan NDC + RGBA). Marks
// the buffer consumed so the next GXBegin starts a fresh frame. Returns the vertex
// count (multiple of 3); *out points at SbImmVtx[count] (== NvkVertex layout).
int sb_gx_imm_take(const SbImmVtx** out) {
    if (out) *out = g_frame_tris.data();
    g_consumed = true;
    return (int)g_frame_tris.size();
}

} // extern "C"
