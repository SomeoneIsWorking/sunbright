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

}  // namespace ngx_imm
