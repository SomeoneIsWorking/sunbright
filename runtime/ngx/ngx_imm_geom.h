#pragma once
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
