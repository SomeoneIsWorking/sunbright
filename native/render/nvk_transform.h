// nvk_transform.h — model-space -> screen projection for the native render path.
//
// Real J3D geometry is MODEL-space; the engine places it with a model-view matrix
// (built via the MTX seam) and the active projection (GXState, via GXGetProjectionv/
// Viewportv). This pure helper runs a decoded NgxVertex through the engine's own
// GXProject (the verified eye->screen transform) and maps the result to Vulkan NDC,
// assuming the viewport fills the render target. Pure + unit-testable.
#pragma once
#include "ngx_mesh.h"   // NgxVertex
#include "nvk.h"        // NvkVertex
#include <dolphin/gx.h> // GXProject

namespace sb::render {

// model pos -> Vulkan NDC xy. mv = model-view (3x4 affine), pm = GXGetProjectionv (7),
// vp = GXGetViewportv (6). Returns NDC in ndc[2]. (Screen px / target size * 2 - 1.)
inline void nvk_project_ndc(const float pos[3], float mv[3][4],
                            const float* pm, const float* vp, float ndc[2]) {
    float sx, sy, sz;
    GXProject(pos[0], pos[1], pos[2], mv, (float*)pm, (float*)vp, &sx, &sy, &sz);
    // vp[0]=left vp[1]=top vp[2]=width vp[3]=height; GXProject folds left/top into sx/sy.
    ndc[0] = (sx / vp[2]) * 2.0f - 1.0f;
    ndc[1] = (sy / vp[3]) * 2.0f - 1.0f;
}

// Expand decoded geometry (model-space NgxVertex + tri indices) into projected
// NvkVertex triangles ready for Nvk::renderTriangles.
inline std::vector<NvkVertex> nvk_project_mesh(
        const std::vector<NgxVertex>& verts, const std::vector<unsigned>& indices,
        float mv[3][4], const float* pm, const float* vp) {
    std::vector<NvkVertex> out;
    out.reserve(indices.size());
    for (unsigned i : indices) {
        const NgxVertex& s = verts[i];
        float ndc[2];
        nvk_project_ndc(s.pos, mv, pm, vp, ndc);
        out.push_back({ ndc[0], ndc[1],
                        s.clr[0][0] / 255.f, s.clr[0][1] / 255.f,
                        s.clr[0][2] / 255.f, s.clr[0][3] / 255.f });
    }
    return out;
}

} // namespace sb::render
