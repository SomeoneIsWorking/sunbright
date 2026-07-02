// sb_parity_dump.h — VALUE-TRACK parity dump for the native renderer's per-frame state.
//
// Part of the Dolphin-GX vs PC-native parity SWEEP. Two tracks, by design:
//   • PIXEL track (vs the Dolphin-GX oracle): the FINAL frame is the only trustworthy
//     reference, because Dolphin's CPU-side intermediate state (xfmem/GP regs) is
//     async-lagged (project rule). Compared per-region by tools/render/parity_sweep.py
//     `image` mode over the dumped PPM/PNG frames.
//   • VALUE track (THIS file): the native engine's own per-frame GEOMETRY + LIGHTING +
//     XFB-region state, emitted as one JSON line per frame. Verified two ways by
//     parity_sweep.py: `check` (invariants — NaN / world-coord leak / degenerate AABB /
//     missing lights / black-or-white XFB) and `diff` (A/B between two runs — regression
//     detection, e.g. before/after a renderer change at the same frame index). The oracle
//     for the value track is the SPEC + self-consistency, not Dolphin's lagged state.
//
// Schema is intentionally compact (one JSONL object/frame). Pure formatting over the data
// the present already has; no Vulkan/GX calls of its own (caller passes the readback).
#pragma once
#include "gx_geom.h"
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>

// TApplication::mAppState reader — defined in native/src/sb_parity_appstate.cpp (sms-native
// lib, which has the sms include path). NON-inline so sb_parity_emit's call site in
// sms-render creates a hard link dependency, forcing the linker to pull in the sms-native
// TU that defines it (a static-init constructor would otherwise be stripped by --gc-sections).
extern "C" unsigned sb_get_app_state();

namespace sb::render {

// Per-light + channel state the caller fills from the GX accessors (sb_gx_get_lights /
// _chan_amb / _chan_matcolor) — passed in so this header stays GX-free + unit-testable.
struct SbParityLights {
    int   n = 0;
    float pos[8][3] = {};
    float col[8][3] = {};
    float amb[3]    = {0,0,0};
    float matc[4]   = {1,1,1,1};
};

// The CROSS-ENGINE comparable state (pure Dolphin vs sms-boot): renderer-independent game
// output. projType + proj[6] (GXSetProjection) + viewport[6] (GXSetViewport) are exact game
// state; both engines set them identically. The frame geometry AGGREGATE (on-screen vert count,
// NDC AABB, position checksum) is the renderer-neutral geometry signal — the batch/shape grouping
// differs between engines, but the projected on-screen geometry must match if both are faithful.
struct SbParityProj {
    int   type = 0;     // 0 = perspective, 1 = ortho
    float proj[6] = {};
    float vp[6]   = {};
};

// Emit one JSON line describing this frame. `verts` are CLIP-space (xyzw); NDC = xyz/w.
// `fb` is the RGBA8 readback (fbw*fbh*4) — the XFB whose region stats we summarise.
// `pass` tags WHICH render pass this line is (cross-engine pass alignment — see gx_capture.cpp).
// The native parity dump captures the 3D J3D scene only, so it is the "scene" (perspective) pass;
// parity_sweep aligns it to the oracle's matching scene-pass line. Defaulted so existing callers
// (and the value-track A/B) keep working unchanged.
inline void sb_parity_emit(std::FILE* f, int frame, const float clear[4],
                           const NvkTevVertex* verts, int nverts,
                           const NvkTevBatch* batches, int nbatch,
                           const SbParityLights& L, const SbParityProj& P,
                           const uint8_t* fb, int fbw, int fbh,
                           const char* pass = "scene") {
    if (!f) return;
    // native g_verts is a TRIANGLE LIST (verts in triples), so triangles = nverts/3. This is the
    // triangulation-invariant metric comparable to the oracle's "tris" (strips/fans/quads reduced to
    // triangles) — the reliable geometry-parity number, unlike raw-vs-list vert counts.
    std::fprintf(f, "{\"frame\":%d,\"pass\":\"%s\",\"clear\":[%.3f,%.3f,%.3f,%.3f],\"nverts\":%d,\"tris\":%d,\"nbatch\":%d",
                 frame, pass, clear[0], clear[1], clear[2], clear[3], nverts, nverts / 3, nbatch);

    // Game-state fingerprint (TApplication::mAppState — one of APP_STATE_{WAIT..MENU}).
    // parity_sweep gates drawdiff on THIS matching between engines. A silent state mismatch
    // (native at APP_STATE_TITLE, oracle at APP_STATE_GAMEPLAY, both settled) was shipping
    // meaningless SBS captures before this — now it exits STATE-MISMATCH.
    // Read directly from the native TApplication object: gpApplication.mAppState.
    std::fprintf(f, ",\"appState\":%u", sb_get_app_state());

    // Projection + viewport (exact cross-engine game state).
    std::fprintf(f, ",\"projType\":%d,\"proj\":[%.5f,%.5f,%.5f,%.5f,%.5f,%.5f],\"vp\":[%.1f,%.1f,%.1f,%.1f,%.4f,%.4f]",
                 P.type, P.proj[0],P.proj[1],P.proj[2],P.proj[3],P.proj[4],P.proj[5],
                 P.vp[0],P.vp[1],P.vp[2],P.vp[3],P.vp[4],P.vp[5]);

    // Frame geometry aggregate (renderer-neutral): on-screen vert count, NDC AABB over the
    // on-screen verts, and a position checksum over ALL finite verts. The cross-engine geometry
    // divergence signal (a skinning/pose/camera bug moves these).
    {
        float gxmn=1e30f,gxmx=-1e30f,gymn=1e30f,gymx=-1e30f,gzmn=1e30f,gzmx=-1e30f;
        double gcks = 0, gcol = 0; int gon = 0, gnan = 0;
        for (int i = 0; i < nverts; ++i) {
            const NvkTevVertex& v = verts[i];
            if (!(std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z)&&std::isfinite(v.w))) { ++gnan; continue; }
            // On-screen = w>0 and screen X,Y in [-1,1]. NDC Z is NOT in the test: its range
            // differs across engines (sms-boot remaps to Vulkan [0,1], the GX path keeps raw
            // clip z), so it is not cross-engine comparable; we report it but never gate on it.
            if (v.w > 1e-5f) { const float nx=v.x/v.w, ny=v.y/v.w, nz=v.z/v.w;
                if (nx>=-1.f&&nx<=1.f&&ny>=-1.f&&ny<=1.f) { ++gon;
                    if(nx<gxmn)gxmn=nx; if(nx>gxmx)gxmx=nx; if(ny<gymn)gymn=ny; if(ny>gymx)gymx=ny;
                    if(nz<gzmn)gzmn=nz; if(nz>gzmx)gzmx=nz; gcks += (double)nx*1.0+(double)ny*1.1;
                    gcol += (double)v.rgba[0]+(double)v.rgba[1]*1.1+(double)v.rgba[2]*1.2; } }
        }
        if (!gon) { gxmn=gxmx=gymn=gymx=gzmn=gzmx=0.f; }
        std::fprintf(f, ",\"geom\":{\"onscr\":%d,\"nan\":%d,\"ndc\":[%.4f,%.4f,%.4f,%.4f,%.4f,%.4f],\"cks\":%.3f,\"colcks\":%.3f}",
                     gon, gnan, gxmn,gxmx,gymn,gymx,gzmn,gzmx, gcks, gcol);
    }

    // Lighting.
    std::fprintf(f, ",\"lights\":{\"n\":%d,\"l\":[", L.n);
    for (int i = 0; i < L.n && i < 8; ++i)
        std::fprintf(f, "%s{\"p\":[%.1f,%.1f,%.1f],\"c\":[%.3f,%.3f,%.3f]}", i ? "," : "",
                     L.pos[i][0], L.pos[i][1], L.pos[i][2], L.col[i][0], L.col[i][1], L.col[i][2]);
    std::fprintf(f, "]},\"amb\":[%.3f,%.3f,%.3f],\"matc\":[%.3f,%.3f,%.3f,%.3f]",
                 L.amb[0], L.amb[1], L.amb[2], L.matc[0], L.matc[1], L.matc[2], L.matc[3]);

    // Per-batch geometry. We dump CLIP-space AABB (pre-divide, ALWAYS finite — NDC/post-divide
    // is meaningless near w=0 where the GPU clips, and the capture now emits unclipped geometry),
    // an on-screen vertex count (w>0 and |ndc|<=1 on all axes — the visible fraction), a checksum,
    // and a "bad" count = NaN/inf ONLY (the one unambiguous corruption signal; large clip coords
    // are normal for distant/near-plane geometry and must NOT be flagged).
    std::fprintf(f, ",\"batches\":[");
    for (int bi = 0; bi < nbatch; ++bi) {
        const NvkTevBatch& b = batches[bi];
        float cxmn=1e30f,cxmx=-1e30f,cymn=1e30f,cymx=-1e30f,czmn=1e30f,czmx=-1e30f,cwmn=1e30f,cwmx=-1e30f;
        double cks = 0; int bad = 0; uint32_t cnt = 0, onscr = 0;
        for (uint32_t i = b.vstart; i < b.vstart + b.vcount && (int)i < nverts; ++i) {
            const NvkTevVertex& v = verts[i];
            if (!(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w))) { ++bad; continue; }
            cks += (double)v.x*1.0 + (double)v.y*1.1 + (double)v.z*1.2 + (double)v.w*1.3;
            if (v.x<cxmn)cxmn=v.x; if (v.x>cxmx)cxmx=v.x; if (v.y<cymn)cymn=v.y; if (v.y>cymx)cymx=v.y;
            if (v.z<czmn)czmn=v.z; if (v.z>czmx)czmx=v.z; if (v.w<cwmn)cwmn=v.w; if (v.w>cwmx)cwmx=v.w;
            if (v.w > 1e-5f) { const float nx=v.x/v.w, ny=v.y/v.w, nz=v.z/v.w;
                if (nx>=-1.f&&nx<=1.f&&ny>=-1.f&&ny<=1.f&&nz>=0.f&&nz<=1.f) ++onscr; }
            ++cnt;
        }
        if (!cnt) { cxmn=cxmx=cymn=cymx=czmn=czmx=cwmn=cwmx=0.f; }
        std::fprintf(f, "%s{\"k\":\"%llx\",\"vc\":%u,\"onscr\":%u,\"z\":[%u,%u,%u],\"bm\":[%u,%u,%u],"
                     "\"cU\":%u,\"aU\":%u,\"ntex\":%d,\"ph\":%u,"
                     "\"clip\":[%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.3f,%.3f],\"cks\":%.3f,\"bad\":%d",
                     bi ? "," : "", (unsigned long long)b.shaderKey, b.vcount, onscr,
                     b.z_test, b.z_func, b.z_write, b.blend_mode, b.src_factor, b.dst_factor,
                     b.color_update, b.alpha_update,
                     [&]{ int n=0; for(int t=0;t<8;++t) if(b.tex[t].rgba)++n; return n; }(),
                     (unsigned)b.phase,
                     cxmn,cxmx,cymn,cymx,czmn,czmx,cwmn,cwmx, cks, bad);
        // Per-batch LIGHTING snapshot — same fields as the oracle's per-draw light snap in
        // gx_capture.cpp (cc/amb/matc/lm/lights). This is the cross-engine join anchor for the
        // overbright wash diagnostic: pin a shape by (shaderKey, phase, vc) and diff the raster-
        // stage inputs to find which pipeline stage diverges (lighting port vs shader vs blend).
        std::fprintf(f, ",\"cc\":%u,\"amb\":[%.3f,%.3f,%.3f],\"matc\":[%.3f,%.3f,%.3f,%.3f],\"lm\":%u,\"lights\":[",
                     b.dbg_chan_ctrl, b.dbg_amb[0], b.dbg_amb[1], b.dbg_amb[2],
                     b.dbg_matc[0], b.dbg_matc[1], b.dbg_matc[2], b.dbg_matc[3], b.dbg_light_mask);
        int lem = 0;
        for (int li = 0; li < 8; ++li) {
            if (!(b.dbg_light_mask & (1u << li))) continue;
            std::fprintf(f, "%s{\"i\":%d,\"p\":[%.1f,%.1f,%.1f],\"c\":[%.3f,%.3f,%.3f]}",
                         lem++ ? "," : "", li,
                         b.dbg_light_pos[li][0], b.dbg_light_pos[li][1], b.dbg_light_pos[li][2],
                         b.dbg_light_col[li][0], b.dbg_light_col[li][1], b.dbg_light_col[li][2]);
        }
        std::fprintf(f, "]}");
    }
    std::fprintf(f, "]");

    // XFB region summary: a 4x4 grid of mean RGB + overall brightness (the value-track
    // proxy for the final image; the PIXEL track does the real per-region image diff).
    if (fb && fbw > 0 && fbh > 0) {
        double allsum = 0; std::fprintf(f, ",\"xfb\":{\"grid\":[");
        for (int gy = 0; gy < 4; ++gy) for (int gx = 0; gx < 4; ++gx) {
            const int x0 = gx*fbw/4, x1 = (gx+1)*fbw/4, y0 = gy*fbh/4, y1 = (gy+1)*fbh/4;
            double r=0,g=0,bl=0; uint32_t n=0;
            for (int y=y0;y<y1;++y) for (int x=x0;x<x1;++x) {
                const uint8_t* p = fb + ((size_t)y*fbw + x)*4;
                r+=p[0]; g+=p[1]; bl+=p[2]; ++n;
            }
            if (!n) n=1;
            std::fprintf(f, "%s[%.1f,%.1f,%.1f]", (gx||gy)?",":"", r/n, g/n, bl/n);
            allsum += (r+g+bl)/(3.0*n);
        }
        std::fprintf(f, "],\"bright\":%.2f}", allsum/16.0);
    }
    std::fprintf(f, "}\n");
    std::fflush(f);
}

} // namespace sb::render
