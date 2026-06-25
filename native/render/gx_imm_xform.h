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

// Forward-declared so the pure transform/triangulation unit (and its tests) need no
// renderer headers; the present layer dereferences it (sms_boot_present.cpp).
struct NgxTevState;

namespace sb::render {

// A captured immediate-mode vertex BEFORE transform: model-space position + RGBA(0..1)
// + texcoord0 UV (0 when the prim is untextured; J2D textured panes set it).
struct SbImmRawVtx { float x, y, z; float r, g, b, a; float u, v; };

// A transformed vertex: Vulkan NDC position + RGBA(0..1) + texcoord0 UV. The present
// layer renders it directly (the 3D/2D NvkTevVertex carries the same fields).
struct SbImmVtx { float x, y, z; float r, g, b, a; float u, v; };

// A captured 2D draw batch: a run of triangles (into the flat list sb_gx_imm_take_batches
// returns) that share one bound texture, or none. `textured` panes carry the resolved GX
// texture descriptor (snapshotted from the bound texmap-0 at GXBegin) so the present layer
// can decode it with sb_tex_decode and bind it; untextured prims (gradient, solid window
// fill) are a colour-only batch.
struct SbImmBatch {
    unsigned vstart, vcount;        // [vstart, vstart+vcount) into the flat vertex list
    bool     textured;
    const void* image;              // GX image data ptr (valid iff textured)
    unsigned short w, h;            // logical texture dims
    unsigned char  fmt, wrapS, wrapT, linear;
    const void* tlut;               // palette ptr for CI formats (nullptr otherwise)
    int tlutfmt;                    // SbTlutFormat for the palette
    // Blend state captured live at GXBegin (GXSetBlendMode): GX_BM_* type + GXBlendFactor
    // src/dst. -1 = "not captured, use the present-layer default" (so existing colour-only
    // paths and tests that don't set these keep the old SRCALPHA/INVSRCALPHA behaviour).
    signed char blendType, blendSrc, blendDst;
    // GXSetColorUpdate: false = the draw writes NO colour (an alpha-plane-only pass, e.g.
    // SMS_FillScreenAlpha / the TModelWaterManager dst-alpha mask setup). The present layer
    // drops its colour contribution — otherwise the invisible fullscreen quad washes the
    // scene (the Delfino "white wash"). True for every ordinary draw.
    bool colorUpdate = true;
    // The FULL GX TEV combiner captured at GXBegin (state().tev → NgxTevState). When set
    // (num_stages>0), the present layer generates the real per-stage combiner fragment and
    // honors it — instead of the legacy texture-modulate/passthrough approximations, which
    // are wrong for J2DPicture's intensity→black/white ramp (the white-letterbox-bar bug).
    // nullptr / num_stages==0 ⇒ fall back to the legacy modulate/passthrough path. Points
    // into a per-frame store owned by gx_imm_impl.cpp (stable until the present consumes).
    const NgxTevState* tev = nullptr;
};

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
    o.u = o.v = 0.0f;
    return o;
}

// Clip-space (homogeneous, PRE-divide) position. gl_Position = vec4(x,y,z,w); the GPU does
// the perspective divide → perspective-correct attribute interpolation AND hardware near/side
// clipping for free. By construction (x/w, y/w, z/w) == imm_project_eye(...).{x,y,z} (the NDC
// the old CPU-divide path produced), so depth and on-screen position are bit-for-bit the same;
// only the WRONG affine (w=1) interpolation + the hand-rolled NDC clipper are removed.
struct SbImmClip { float x, y, z, w; };

inline SbImmClip imm_project_eye_clip(float ex, float ey, float ez, int projType,
                                      const float projMtx[6], const float vp[6]) {
    SbImmClip o;
    if (projType == 0) {               // perspective (GX_PERSPECTIVE == 0)
        const float xc = ex*projMtx[0] + ez*projMtx[1];
        const float yc = ey*projMtx[2] + ez*projMtx[3];
        const float zc = projMtx[5] + ez*projMtx[4];
        const float w  = -ez;          // clip w (>0 in front of camera; the GPU near-clips w→0)
        const float ox = (vp[2] != 0.0f) ? 2.0f*vp[0]/vp[2] : 0.0f;   // viewport x-offset (NDC)
        const float oy = (vp[3] != 0.0f) ? 2.0f*vp[1]/vp[3] : 0.0f;   // viewport y-offset (NDC)
        o.x =  xc + ox*w;              // x/w = 2*vp0/vp2 + xc/(-ez)  == imm_project_eye ndc_x
        o.y = -yc + oy*w;              // y/w = 2*vp1/vp3 - yc/(-ez)  == imm_project_eye ndc_y (Y down)
        o.z = vp[5]*w + zc*(vp[5]-vp[4]); // z/w = vp5 + zc*(vp5-vp4)/(-ez) == ndc_z (Vulkan [0,1])
        o.w =  w;
    } else {                            // orthographic — wc already 1, NDC == clip
        SbImmVtx p = imm_project_eye(ex, ey, ez, projType, projMtx, vp);
        o.x = p.x; o.y = p.y; o.z = p.z; o.w = 1.0f;
    }
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
    o.u = v.u; o.v = v.v;
    return o;
}

// Scale a raw fixed-point/integer immediate texcoord component to a normalized UV using
// the bound TEX0 vertex-attr format (GXSetVtxAttrFmt: component type + frac bits). This
// is the exact GX dequant the GP applies: fixed types divide by 2^frac (signed types
// sign-extend their bit width first); F32 components arrive pre-normalized (see the float
// hooks). `bits` is the raw value the GXTexCoord* writer pushed, zero-extended; `type` is
// GXCompType (0=U8 1=S8 2=U16 3=S16 4=F32); `width` is 8 or 16. Pure + unit-tested.
//
// J2D's default TEX0 fmt is (GX_U16, frac 15) (J2DGrafContext): so GXTexCoord1s16(0x8000)
// -> 32768/2^15 = 1.0 and 0x0000 -> 0.0, exactly the corner UVs J2DPicture/J2DWindow emit.
inline float imm_texcoord_scale(unsigned bits, int type, int frac, int width) {
    long v;
    if (type == 1 /*S8*/  && (bits & 0x80))   v = (long)bits - 0x100;
    else if (type == 3 /*S16*/ && (bits & 0x8000)) v = (long)bits - 0x10000;
    else v = (long)bits;
    (void)width;
    return (float)v / (float)(1u << (frac & 31));
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
