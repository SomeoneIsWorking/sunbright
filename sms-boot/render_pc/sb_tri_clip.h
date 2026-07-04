// sb_tri_clip.h — shared triangle clipper for the native scene capture (J3D shapes AND the
// procedural sky/backdrop). GX geometry that SURROUNDS the camera (the sky/backdrop dome) has
// triangles straddling ez=0; the perspective divide (w = 1/-ez in imm_project_eye) flips/explodes
// behind-camera verts into garbage NDC, so those triangles get dropped. We clip each triangle:
//   (1) near plane in EYE space (perspective only; Sutherland-Hodgman against ez <= -kNear),
//   (2) project survivors to NDC,
//   (3) the 4 NDC side planes x,y∈[-1,1] (so the huge dome's clip-verts can't reach extreme NDC
//       and get dropped by Vulkan's guard band),
// then fan-triangulate into `out` (a triangle list).
//
// NOTE (perspective-correctness): the side-plane clip interpolates attributes (colour/UV) LINEARLY
// in NDC space, which is perspective-INCORRECT. The error is bounded to the screen edge and is
// invisible for near-uniform sky colour; for textured geometry that straddles a side plane it can
// smear UVs at the very edge. Acceptable for now; a clip-space (pre-divide) clip would fix it.
#pragma once
#include "gx_geom.h"
#include "gx_imm_xform.h"
#include <vector>

namespace sb::render {

// Eye-space vertex + shaded attributes (NvkTevVertex x/y/z are filled in after projection).
struct SbClipEyeVtx { float ex, ey, ez; NvkTevVertex tv; };

inline void sb_clip_emit_tri(const SbClipEyeVtx tri[3], int projType,
                             const float proj[6], const float vp[6],
                             std::vector<NvkTevVertex>& out) {
    auto lerp_cv = [](const SbClipEyeVtx& A, const SbClipEyeVtx& B, float t) -> SbClipEyeVtx {
        SbClipEyeVtx R; R.tv = NvkTevVertex{};   // zero-init NDC slots (filled at projection)
        R.ex = A.ex + t*(B.ex-A.ex); R.ey = A.ey + t*(B.ey-A.ey); R.ez = A.ez + t*(B.ez-A.ez);
        for (int k = 0; k < 4; ++k) { R.tv.rgba[k]  = A.tv.rgba[k]  + t*(B.tv.rgba[k]-A.tv.rgba[k]);
                                      R.tv.rgba1[k] = A.tv.rgba1[k] + t*(B.tv.rgba1[k]-A.tv.rgba1[k]); }
        for (int u = 0; u < 8; ++u) { R.tv.uv[u][0] = A.tv.uv[u][0] + t*(B.tv.uv[u][0]-A.tv.uv[u][0]);
                                      R.tv.uv[u][1] = A.tv.uv[u][1] + t*(B.tv.uv[u][1]-A.tv.uv[u][1]); }
        return R;
    };
    // (1) near-plane clip in EYE space — perspective only (ortho has wc=1, no singularity, and its
    // clip planes aren't ez-based, so a near-clip there would wrongly drop valid geometry).
    SbClipEyeVtx ep[8]; int np = 0;
    if (projType == 0) {
        const float kNear = 1.0f;   // small, just in front of the camera; dodges w=1/-ez at ez=0
        for (int e = 0; e < 3; ++e) {
            const SbClipEyeVtx& A = tri[e]; const SbClipEyeVtx& B = tri[(e+1) % 3];
            const bool ai = A.ez <= -kNear, bi = B.ez <= -kNear;
            if (ai) ep[np++] = A;
            if (ai != bi) { const float t = (-kNear - A.ez) / (B.ez - A.ez); ep[np++] = lerp_cv(A, B, t); }
        }
        if (np < 3) return;
    } else { ep[0] = tri[0]; ep[1] = tri[1]; ep[2] = tri[2]; np = 3; }

    // (2) project to NDC. ep has at most 4 verts -> the 4 side-clip planes add ≤1 each -> ≤8; pa/pb
    // sized 10 for margin.
    NvkTevVertex pa[10], pb[10];
    int pn = 0;
    for (int i = 0; i < np; ++i) {
        SbImmVtx p = imm_project_eye(ep[i].ex, ep[i].ey, ep[i].ez, projType, proj, vp);
        pa[pn] = ep[i].tv; pa[pn].x = p.x; pa[pn].y = p.y; pa[pn].z = p.z; ++pn;
    }

    // (3) NDC side-plane clip x,y∈[-1,1].
    auto lerp_ndc = [](const NvkTevVertex& A, const NvkTevVertex& B, float t) -> NvkTevVertex {
        NvkTevVertex R;
        R.x = A.x + t*(B.x-A.x); R.y = A.y + t*(B.y-A.y); R.z = A.z + t*(B.z-A.z);
        for (int k = 0; k < 4; ++k) { R.rgba[k]  = A.rgba[k]  + t*(B.rgba[k]-A.rgba[k]);
                                      R.rgba1[k] = A.rgba1[k] + t*(B.rgba1[k]-A.rgba1[k]); }
        for (int u = 0; u < 8; ++u) { R.uv[u][0] = A.uv[u][0] + t*(B.uv[u][0]-A.uv[u][0]);
                                      R.uv[u][1] = A.uv[u][1] + t*(B.uv[u][1]-A.uv[u][1]); }
        return R;
    };
    auto clip_axis = [&](NvkTevVertex* in, int n, int axis, float sign, float lim, NvkTevVertex* o) -> int {
        int m = 0;
        for (int i = 0; i < n; ++i) {
            const NvkTevVertex& A = in[i]; const NvkTevVertex& B = in[(i+1) % n];
            const float av = (axis ? A.y : A.x), bv = (axis ? B.y : B.x);
            const bool ina = av * sign <= lim * sign, inb = bv * sign <= lim * sign;
            if (ina) o[m++] = A;
            if (ina != inb) { const float t = (lim - av) / (bv - av); o[m++] = lerp_ndc(A, B, t); }
        }
        return m;
    };
    pn = clip_axis(pa, pn, 0, +1.f, 1.f, pb);   if (pn < 3) return;   // x <= 1
    pn = clip_axis(pb, pn, 0, -1.f, -1.f, pa);  if (pn < 3) return;   // x >= -1
    pn = clip_axis(pa, pn, 1, +1.f, 1.f, pb);   if (pn < 3) return;   // y <= 1
    pn = clip_axis(pb, pn, 1, -1.f, -1.f, pa);  if (pn < 3) return;   // y >= -1

    // (4) fan-triangulate (winding preserved; raster is cull-none anyway).
    for (int i = 1; i + 1 < pn; ++i) { out.push_back(pa[0]); out.push_back(pa[i]); out.push_back(pa[i+1]); }
}

} // namespace sb::render
