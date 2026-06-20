// gx_impl.cpp — native GX seam, SLICE 1: the transform block (projection/viewport/
// scissor + GXProject). "Rebuild as a PC game": GXSet* capture into the native GXState
// (gx_state.h); the GameCube FIFO / XF-register writes the decomp does are DROPPED —
// the native renderer reads GXState, there is no command stream to emulate.
//
// GXProject is ported verbatim from the decomp (GXTransform.c) — pure eye->screen math.
// GXSetProjection/GXSetViewport/GXSetScissor port only the gx-struct writes (not the
// GX_WRITE_* FIFO pokes). GXGetProjectionv/GXGetViewportv read the state back in the
// exact pm[]/vp[] layout GXProject expects. The full GX surface (TEV/blend/Z/tex/draw/
// present) lands in later slices, routed to the ngx renderer.

#include <dolphin/gx.h>
#include "gx_state.h"

namespace sb::platform::gx {
GXState& state() { static GXState g_gx{}; return g_gx; }
}

using sb::platform::gx::state;

extern "C" {

// Eye->screen projection (verbatim from the decomp; self-contained pure math).
void GXProject(f32 x, f32 y, f32 z, f32 mtx[3][4], f32* pm, f32* vp,
               f32* sx, f32* sy, f32* sz) {
    f32 ex = mtx[0][3] + ((mtx[0][2]*z) + ((mtx[0][0]*x) + (mtx[0][1]*y)));
    f32 ey = mtx[1][3] + ((mtx[1][2]*z) + ((mtx[1][0]*x) + (mtx[1][1]*y)));
    f32 ez = mtx[2][3] + ((mtx[2][2]*z) + ((mtx[2][0]*x) + (mtx[2][1]*y)));
    f32 xc, yc, zc, wc;
    if (pm[0] == 0.0f) {           // perspective (pm[0] = GX_PERSPECTIVE = 0)
        xc = (ex*pm[1]) + (ez*pm[2]);
        yc = (ey*pm[3]) + (ez*pm[4]);
        zc = pm[6] + (ez*pm[5]);
        wc = 1.0f / -ez;
    } else {                        // orthographic
        xc = pm[2] + (ex*pm[1]);
        yc = pm[4] + (ey*pm[3]);
        zc = pm[6] + (ez*pm[5]);
        wc = 1.0f;
    }
    *sx = (vp[2]/2.0f) + (vp[0] + (wc * (xc * vp[2]/2.0f)));
    *sy = (vp[3]/2.0f) + (vp[1] + (wc * (-yc * vp[3]/2.0f)));
    *sz = vp[5] + (wc * (zc * (vp[5] - vp[4])));
}

void GXSetProjection(f32 mtx[4][4], GXProjectionType type) {
    auto& g = state();
    g.projType   = type;
    g.projMtx[0] = mtx[0][0];
    g.projMtx[2] = mtx[1][1];
    g.projMtx[4] = mtx[2][2];
    g.projMtx[5] = mtx[2][3];
    if (type == GX_ORTHOGRAPHIC) {
        g.projMtx[1] = mtx[0][3];
        g.projMtx[3] = mtx[1][3];
    } else {
        g.projMtx[1] = mtx[0][2];
        g.projMtx[3] = mtx[1][2];
    }
}

void GXGetProjectionv(f32* ptr) {
    auto& g = state();
    ptr[0] = (f32)g.projType;
    ptr[1] = g.projMtx[0];
    ptr[2] = g.projMtx[1];
    ptr[3] = g.projMtx[2];
    ptr[4] = g.projMtx[3];
    ptr[5] = g.projMtx[4];
    ptr[6] = g.projMtx[5];
}

void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz,
                         u32 field) {
    if (field == 0) top -= 0.5f;   // matches the decomp (de-jitter for field 0)
    auto& g = state();
    g.vpLeft = left; g.vpTop = top; g.vpWd = wd; g.vpHt = ht;
    g.vpNearz = nearz; g.vpFarz = farz;
}

void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
    GXSetViewportJitter(left, top, wd, ht, nearz, farz, 1u);
}

void GXGetViewportv(f32* vp) {
    auto& g = state();
    vp[0] = g.vpLeft; vp[1] = g.vpTop; vp[2] = g.vpWd; vp[3] = g.vpHt;
    vp[4] = g.vpNearz; vp[5] = g.vpFarz;
}

void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht) {
    auto& g = state();
    g.scLeft = left; g.scTop = top; g.scWd = wd; g.scHt = ht;
}

} // extern "C"
