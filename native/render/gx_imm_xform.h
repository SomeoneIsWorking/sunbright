#pragma once
// gx_imm_xform.h — PURE immediate-mode 2D/3D vertex transform + triangulation for
// the native renderer's GX immediate-mode capture (SLICE 2 of renderer-attach).
//
// The GameCube immediate-mode primitives (GXBegin/GXPosition*/GXColor*/GXEnd) have no
// J3D object the scene-capture path can read, so we capture them at the GX seam and
// transform them HERE: model-space position -> (current pos matrix) -> (captured
// projection) -> Vulkan NDC, exactly mirroring the decomp's GXProject (GXTransform.c).
// This is a pure unit (no Vulkan, no GXState, no globals) so render_test can assert it
// against hand-computed ground truth — the "move a number" gate the renderer rules
// require before any pixel is trusted.
//
// Y note: the GameCube clip space has +Y up; nvk uses a standard Vulkan viewport
// (+Y down). So Vulkan NDC y = -(GC clip y / w). Depth maps GC clip z/w in [-1,1]
// to Vulkan [0,1] via *0.5+0.5 (2D overlays use z=0 -> 0.5; depth test is usually off).
#include <vector>

namespace sb::render {

// A captured immediate-mode vertex BEFORE transform: model-space position + RGBA(0..1).
struct SbImmRawVtx { float x, y, z; float r, g, b, a; };

// A transformed vertex: Vulkan NDC position + RGBA(0..1). Byte-identical layout to
// NvkVertex (asserted where the two meet) so the present layer can render it directly.
struct SbImmVtx { float x, y, z; float r, g, b, a; };

// GX primitive ids (GXEnum.h) — only the ones immediate mode emits.
enum {
    SB_GX_QUADS         = 0x80,
    SB_GX_TRIANGLES     = 0x90,
    SB_GX_TRIANGLESTRIP = 0x98,
    SB_GX_TRIANGLEFAN   = 0xA0,
};

// Transform a model-space immediate vertex to Vulkan NDC using the EXACT decomp math:
// model -> eye (current pos matrix) -> clip -> SCREEN (GXProject, GXTransform.c) using
// BOTH the captured projection AND viewport, then screen-pixel -> Vulkan NDC. Going
// through the real GXProject (not a simplified clip) means we reproduce whatever the
// game set, including SMS's quirky C_MTXOrtho arg order (width/height land in the t/b
// vs l/r slots) — we never have to special-case it.
//
//   projType: 0 = GX_PERSPECTIVE, 1 = GX_ORTHOGRAPHIC.
//   projMtx[0..5] = the decomp's gx->projMtx (= GXProject's pm[1..6]).
//   posMtx       = current 3x4 position matrix.
//   vp[0..5]     = viewport {left, top, width, height, nearz, farz} (GXGetViewportv).
// Eye-space position -> Vulkan NDC (the second half of imm_project, split out so callers
// that clip in EYE space against the near plane — sky / camera-surrounding geometry whose
// triangles straddle ez=0 — can project the clipped eye-space verts directly).
inline SbImmVtx imm_project_eye(float ex, float ey, float ez, int projType,
                                const float projMtx[6], const float vp[6]) {
    // eye -> clip (pm[1..6] == projMtx[0..5]; mirrors GXProject's gx_impl.cpp port)
    float xc, yc, zc, wc;
    if (projType == 0) {               // perspective (GX_PERSPECTIVE == 0)
        xc = ex*projMtx[0] + ez*projMtx[1];
        yc = ey*projMtx[2] + ez*projMtx[3];
        zc = projMtx[5] + ez*projMtx[4];
        wc = 1.0f / -ez;
    } else {                            // orthographic
        xc = projMtx[1] + ex*projMtx[0];
        yc = projMtx[3] + ey*projMtx[2];
        zc = projMtx[5] + ez*projMtx[4];
        wc = 1.0f;
    }
    const float sx = vp[2]/2.0f + vp[0] + wc * (xc * vp[2]/2.0f);
    const float sy = vp[3]/2.0f + vp[1] + wc * (-yc * vp[3]/2.0f);
    const float sz = vp[5] + wc * (zc * (vp[5] - vp[4]));
    SbImmVtx o;
    o.x = (vp[2] != 0.0f) ? (2.0f * sx / vp[2] - 1.0f) : 0.0f;
    o.y = (vp[3] != 0.0f) ? (2.0f * sy / vp[3] - 1.0f) : 0.0f;
    o.z = sz;
    o.r = o.g = o.b = o.a = 0.0f;
    return o;
}

inline SbImmVtx imm_project(const SbImmRawVtx& v, int projType, const float projMtx[6],
                            const float posMtx[3][4], const float vp[6]) {
    // model -> eye (current position matrix, 3x4)
    const float ex = posMtx[0][3] + posMtx[0][0]*v.x + posMtx[0][1]*v.y + posMtx[0][2]*v.z;
    const float ey = posMtx[1][3] + posMtx[1][0]*v.x + posMtx[1][1]*v.y + posMtx[1][2]*v.z;
    const float ez = posMtx[2][3] + posMtx[2][0]*v.x + posMtx[2][1]*v.y + posMtx[2][2]*v.z;

    // eye -> clip (pm[1..6] == projMtx[0..5]; mirrors GXProject's gx_impl.cpp port)
    float xc, yc, zc, wc;
    if (projType == 0) {               // perspective (GX_PERSPECTIVE == 0)
        xc = ex*projMtx[0] + ez*projMtx[1];
        yc = ey*projMtx[2] + ez*projMtx[3];
        zc = projMtx[5] + ez*projMtx[4];
        wc = 1.0f / -ez;
    } else {                            // orthographic
        xc = projMtx[1] + ex*projMtx[0];
        yc = projMtx[3] + ey*projMtx[2];
        zc = projMtx[5] + ez*projMtx[4];
        wc = 1.0f;
    }
    // clip -> screen (GXProject): sy uses -yc so screen y grows DOWNWARD.
    const float sx = vp[2]/2.0f + vp[0] + wc * (xc * vp[2]/2.0f);
    const float sy = vp[3]/2.0f + vp[1] + wc * (-yc * vp[3]/2.0f);
    const float sz = vp[5] + wc * (zc * (vp[5] - vp[4]));

    // screen pixel (within the viewport rect) -> Vulkan NDC. The viewport == the full
    // nvk framebuffer, and Vulkan NDC y is DOWN (matching screen y), so no extra flip.
    SbImmVtx o;
    o.x = (vp[2] != 0.0f) ? (2.0f * sx / vp[2] - 1.0f) : 0.0f;
    o.y = (vp[3] != 0.0f) ? (2.0f * sy / vp[3] - 1.0f) : 0.0f;
    o.z = sz;                            // GXProject already maps to [nearz, farz] (0..1)
    o.r = v.r; o.g = v.g; o.b = v.b; o.a = v.a;
    return o;
}

// Triangulate a captured primitive's NDC verts into a triangle list, appended to out.
// GX_QUADS: each 4 verts -> 2 tris (0,1,2)(0,2,3). STRIP/FAN per GX winding.
inline void imm_triangulate(int prim, const SbImmVtx* v, int n, std::vector<SbImmVtx>& out) {
    switch (prim) {
    case SB_GX_QUADS:
        for (int i = 0; i + 3 < n; i += 4) {
            out.push_back(v[i+0]); out.push_back(v[i+1]); out.push_back(v[i+2]);
            out.push_back(v[i+0]); out.push_back(v[i+2]); out.push_back(v[i+3]);
        }
        break;
    case SB_GX_TRIANGLES:
        for (int i = 0; i + 2 < n; i += 3) {
            out.push_back(v[i+0]); out.push_back(v[i+1]); out.push_back(v[i+2]);
        }
        break;
    case SB_GX_TRIANGLESTRIP:
        for (int i = 0; i + 2 < n; ++i) {
            if (i & 1) { out.push_back(v[i+1]); out.push_back(v[i+0]); out.push_back(v[i+2]); }
            else       { out.push_back(v[i+0]); out.push_back(v[i+1]); out.push_back(v[i+2]); }
        }
        break;
    case SB_GX_TRIANGLEFAN:
        for (int i = 1; i + 1 < n; ++i) {
            out.push_back(v[0]); out.push_back(v[i]); out.push_back(v[i+1]);
        }
        break;
    default:
        break;
    }
}

} // namespace sb::render
