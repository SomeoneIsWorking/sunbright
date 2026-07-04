#pragma once
#include <cmath>
// ngx_imm_geom — pure model-space geometry for the GameCube immediate-mode GX
// primitives that ngx must capture (they have no J3D object, so the J3DShape
// capture path misses them). First member: GXDrawCube (reference/sms GXDraw.c).
//
// GXDrawCube issues GXBegin(GX_QUADS, GX_VTXFMT3, 24): six faces × four corners,
// each corner GXPosition3f32(0.57735026 * (±n ±t ±b)) for that face's unit
// (normal, tangent, binormal) basis (GXDrawCubeFace, GXDraw.c:94). With the basis
// vectors being ±unit-axis, every corner lands at (±k, ±k, ±k), k=0.57735026 — a
// unit cube inscribed in the radius-1 sphere. These are MODEL-space; the caller
// transforms by the current position matrix (GX_PNMTX0) + projection.
//
// This is the SHIPPING geometry (the GXDrawCube override calls cube_corners /
// cube_tri_indices); render_test unit `imm_cube` asserts it against the spec.

namespace ngx_imm {

constexpr float kCubeK = 0.57735026f;   // 1/sqrt(3), GXDraw.c's 0.57735026f

// One GXDrawCubeFace: writes its 4 corners (GX_QUADS winding) into out[0..3][3].
// Mirrors GXDraw.c:94 exactly (P0 = k(n+t+b), P1 = k(n-t+b), P2 = k(n-t-b),
// P3 = k(n+t-b)).
inline void cube_face(float out[4][3],
                      float nx, float ny, float nz,
                      float tx, float ty, float tz,
                      float bx, float by, float bz) {
    const float k = kCubeK;
    out[0][0] = k*(nx+tx+bx); out[0][1] = k*(ny+ty+by); out[0][2] = k*(nz+tz+bz);
    out[1][0] = k*(nx-tx+bx); out[1][1] = k*(ny-ty+by); out[1][2] = k*(nz-tz+bz);
    out[2][0] = k*(nx-tx-bx); out[2][1] = k*(ny-ty-by); out[2][2] = k*(nz-tz-bz);
    out[3][0] = k*(nx+tx-bx); out[3][1] = k*(ny+ty-by); out[3][2] = k*(nz+tz-bz);
}

// Fill out[24][3] with the 24 GX_QUADS corners (model space) in GXDrawCube order
// (the six GXDrawCubeFace calls of GXDraw.c:140, same arg order).
inline void cube_corners(float out[24][3]) {
    cube_face(out + 0,  -1, 0, 0,  0, 0,-1,  0, 1, 0);
    cube_face(out + 4,   1, 0, 0,  0, 1, 0,  0, 0,-1);
    cube_face(out + 8,   0,-1, 0, -1, 0, 0,  0, 0, 1);
    cube_face(out + 12,  0, 1, 0,  0, 0, 1, -1, 0, 0);
    cube_face(out + 16,  0, 0,-1,  0,-1, 0,  1, 0, 0);
    cube_face(out + 20,  0, 0, 1,  1, 0, 0,  0,-1, 0);
}

// Triangulate the 6 GX_QUADS (corners 4f..4f+3) into 12 triangles: quad (a,b,c,d)
// → tris (a,b,c),(a,c,d). Fill idx[36].
inline void cube_tri_indices(unsigned idx[36]) {
    int o = 0;
    for (unsigned f = 0; f < 6; f++) {
        const unsigned a = f*4, b = f*4+1, c = f*4+2, d = f*4+3;
        idx[o++] = a; idx[o++] = b; idx[o++] = c;
        idx[o++] = a; idx[o++] = c; idx[o++] = d;
    }
}

// ── GXDrawSphere geometry (reference/sms GXDraw.c:46) ────────────────────────
// GXDrawSphere(numMajor, numMinor) tessellates a unit (radius-1) sphere as numMajor
// latitude bands, each a GX_TRIANGLESTRIP of (numMinor+1)*2 verts. For band i it
// walks j=0..numMinor emitting the OUTER (b) ring vertex then the INNER (a) ring:
//   a=i·π/numMajor, b=a+π/numMajor;  c=j·2π/numMinor;  x=cos c, y=sin c
//   v_out=(x·sin b, y·sin b, cos b)   v_in=(x·sin a, y·sin a, cos a)
// (GXDraw.c emits POS+NRM; the normal == position for a unit sphere, unused here.)
// TSky::perform (Map/Sky.cpp:88) draws GXDrawSphere(8,0x10) as a flat-matColor skybox
// dome — PNMTX0 = scale(100000), so it's a huge camera-centred backdrop, CULL_FRONT
// (seen from inside). MODEL space; the caller transforms by PNMTX0 + projection.
// This is the SHIPPING geometry (ngx_emit_imm_sphere calls it); render_test unit
// `imm_sphere` asserts it against the spec.

inline int sphere_vert_count(int numMajor, int numMinor) { return numMajor * (numMinor + 1) * 2; }
inline int sphere_tri_count (int numMajor, int numMinor) { return numMajor * (numMinor * 2); }

// Fill out[sphere_vert_count][3] with the unit-sphere strip verts, in GXDraw.c order
// (per band, per j: outer ring then inner ring).
inline void sphere_verts(int numMajor, int numMinor, float (*out)[3]) {
    const float majorStep = 3.1415927f / (float)numMajor;
    const float minorStep = 6.2831855f / (float)numMinor;
    int o = 0;
    for (int i = 0; i < numMajor; i++) {
        const float a = i * majorStep, b = a + majorStep;
        const float r0 = std::sin(a), r1 = std::sin(b), z0 = std::cos(a), z1 = std::cos(b);
        for (int j = 0; j <= numMinor; j++) {
            const float c = j * minorStep, x = std::cos(c), y = std::sin(c);
            out[o][0] = x*r1; out[o][1] = y*r1; out[o][2] = z1; o++;   // outer (b) ring — emitted 1st
            out[o][0] = x*r0; out[o][1] = y*r0; out[o][2] = z0; o++;   // inner (a) ring — emitted 2nd
        }
    }
}

// Per-band triangle-strip indices, winding-preserving. Each band's strip of
// S=(numMinor+1)*2 verts begins at base=i·S; strip vert k yields triangle (k,k+1,k+2)
// with GX's alternating strip winding (odd k swaps the first two so the facing stays
// consistent). Fill idx[3*sphere_tri_count].
inline void sphere_tri_indices(int numMajor, int numMinor, unsigned* idx) {
    const int S = (numMinor + 1) * 2;
    int o = 0;
    for (int i = 0; i < numMajor; i++) {
        const unsigned base = (unsigned)(i * S);
        for (int k = 0; k + 2 < S; k++) {
            const unsigned v0 = base + k, v1 = base + k + 1, v2 = base + k + 2;
            if (k & 1) { idx[o++] = v1; idx[o++] = v0; idx[o++] = v2; }
            else       { idx[o++] = v0; idx[o++] = v1; idx[o++] = v2; }
        }
    }
}

// ── Mario occlusion query (native, depth-based) ──────────────────────────────
// The game's GXDrawCube occlusion probe stamps framebuffer alpha 0x10 where a unit
// cube at Mario passes a z-test against the scene, then GXPeekARGB(MarioScreenPos)
// reads it: alpha==0x10 ⇒ Mario NOT occluded. ngx can't serve that mid-frame
// read-after-write from its single end-of-frame readback (the alpha stamp is
// overwritten by later draws). Instead we answer the query DIRECTLY from depth:
// Mario is occluded iff the scene depth at his screen pixel is nearer than the
// occlusion cube's front face. The cube encloses Mario, so its front face is nearer
// than Mario's own body → his body never self-occludes; only real geometry in front
// of the cube does. (Depths are Vulkan-convention: 0=near, 1=far, matching g_efb_depth.)

// The cube's nearest (front-face) Vulkan depth over its projected corners. clip[i] is
// homogeneous clip space (pre-divide); the mesh vertex shader maps z' = clip_z + clip_w
// then Vulkan depth = z'/w = (clip_z + clip_w)/clip_w. Returns 1e9 if no corner is in
// front of the eye (w>0) — i.e. "unknown / far" (treated as not-occluding).
inline float cube_front_depth_vk(const float (*clip)[4], int n) {
    float best = 1e9f;
    for (int i = 0; i < n; i++) {
        const float w = clip[i][3];
        if (w > 1e-4f) {
            const float d = (clip[i][2] + w) / w;   // Vulkan depth, 0=near 1=far
            if (d < best) best = d;
        }
    }
    return best;
}

// Occlusion decision: Mario is occluded iff scene geometry at his pixel is nearer
// (smaller depth) than the cube's front face by more than eps. A negative/invalid
// scene depth (no readback yet) or a far cube (≥1e8) → not occluded.
inline bool imm_occluded(float scene_depth, float cube_front_depth, float eps) {
    if (cube_front_depth >= 1e8f) return false;
    if (scene_depth < 0.f) return false;
    return scene_depth < cube_front_depth - eps;
}

}  // namespace ngx_imm
