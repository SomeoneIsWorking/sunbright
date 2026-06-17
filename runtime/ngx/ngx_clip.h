#pragma once
// ngx_clip — near-plane triangle clip, extracted from ngx_j3d_shape.cpp so it is
// UNIT-TESTED (sunbright-render-test). Clips ONE triangle against the GC near
// plane d = clip.z + clip.w ≥ 0 (= gl_Position.z ≥ 0, the plane Vulkan near-clips
// on). A triangle straddling it cannot be drawn raw: its boundary lands at w≈0,
// the perspective divide x/w → ∞, and it explodes into screen-spanning spikes —
// the title-logo shear / map-sun "white rays" / wash class. Clipping in CLIP
// SPACE places the boundary vertices exactly on the near plane with a finite w.
//
// Vertex = `stride` floats; clip.xyzw MUST be at offset [0..3]; ALL `stride`
// attributes interpolate linearly at each crossing. `out` needs room for 4
// vertices (4*stride floats). Returns the output vertex count:
//   3  wholly in front  (copied through unchanged),
//   0  wholly behind near (dropped),
//   3..4  clipped polygon (caller fan-triangulates np-2 triangles).
// If `nfront` is non-null it receives how many input vertices were in front
// (3 or 0 ⇒ no cut; 1 or 2 ⇒ straddle), for the caller's diagnostics.

// Full frustum clip (all 6 clip-space planes), Sutherland–Hodgman. The near-only clip
// (ngx_clip_near_tri) leaves SCREEN-SPANNING SPIKES for geometry that is off-screen or
// behind the camera: it interpolates a vertex onto the near plane between an in-front and a
// behind-camera vertex, and that interpolated vertex can land far off to the side (NDC.x ≫ 1).
// The real GPU clips that triangle against the LEFT/RIGHT/TOP/BOTTOM/FAR planes too, removing
// the off-screen part cleanly — so a camera transition (objects crossing the near plane) shows
// nothing extra. ngx must do the same on the CPU before emit, or those interpolated near-plane
// verts survive as visible slivers (the title-logo / file-select-transition "shred").
//
// Clips in CLIP SPACE against the 6 half-spaces (inside = a·x+b·y+c·z+d·w ≥ 0), GC convention
// (NDC z ∈ [-1,0]; shader maps z' = z+w so Vulkan near = z+w ≥ 0, far = z ≤ 0):
//   left  x+w≥0   right -x+w≥0   bottom y+w≥0   top -y+w≥0   near z+w≥0   far -z≥0
// Vertex = `stride` floats, clip.xyzw at [0..3], ALL attributes interpolate. Output buffer
// `out` needs room for up to 9 vertices (9*stride floats); returns the polygon vertex count
// (0 if fully clipped, else 3..9 — caller fan-triangulates np-2 triangles). Lossless for
// fully-inside triangles (they pass through, possibly reordered).
inline int ngx_clip_frustum_tri(const float* in, int stride, float* out) {
    static const float planes[6][4] = {
        { 1, 0, 0, 1}, {-1, 0, 0, 1},   // left, right
        { 0, 1, 0, 1}, { 0,-1, 0, 1},   // bottom, top
        { 0, 0, 1, 1}, { 0, 0,-1, 0},   // near (z+w≥0), far (-z≥0)
    };
    // Two ping-pong polygon buffers, max 9 verts each.
    float bufA[9 * 24], bufB[9 * 24];   // 24 = max stride used by the mesh path (VW)
    if (stride > 24) return 0;          // guard: buffers sized for the mesh vertex
    float* cur = bufA; float* nxt = bufB;
    int np = 3;
    for (int i = 0; i < 3 * stride; i++) cur[i] = in[i];
    for (int p = 0; p < 6; p++) {
        const float a = planes[p][0], b = planes[p][1], c = planes[p][2], d = planes[p][3];
        int nn = 0;
        for (int e = 0; e < np && nn < 9; e++) {
            const int e2 = (e + 1) % np;
            const float* va = &cur[e * stride];
            const float* vb = &cur[e2 * stride];
            const float da = a*va[0] + b*va[1] + c*va[2] + d*va[3];
            const float db = a*vb[0] + b*vb[1] + c*vb[2] + d*vb[3];
            if (da >= 0.0f) { for (int k = 0; k < stride; k++) nxt[nn*stride+k] = va[k]; nn++; }
            if ((da >= 0.0f) != (db >= 0.0f) && nn < 9) {
                const float s = da / (da - db);
                for (int k = 0; k < stride; k++) nxt[nn*stride+k] = va[k] + s * (vb[k] - va[k]);
                nn++;
            }
        }
        np = nn;
        float* tmp = cur; cur = nxt; nxt = tmp;
        if (np < 3) return 0;
    }
    for (int i = 0; i < np * stride; i++) out[i] = cur[i];
    return np;
}

inline int ngx_clip_near_tri(const float* in, int stride, float* out,
                             int* nfront = nullptr) {
    float d[3];
    int nf = 0;
    for (int e = 0; e < 3; e++) {
        d[e] = in[e * stride + 2] + in[e * stride + 3];   // d = clip.z + clip.w
        if (d[e] >= 0.0f) nf++;
    }
    if (nfront) *nfront = nf;
    if (nf == 3) { for (int i = 0; i < 3 * stride; i++) out[i] = in[i]; return 3; }
    if (nf == 0) return 0;

    // Sutherland–Hodgman against the single near plane (d ≥ 0 keeps).
    int np = 0;
    for (int e = 0; e < 3 && np < 4; e++) {
        const int e2 = (e + 1) % 3;
        const float da = d[e], db = d[e2];
        if (da >= 0.0f) {
            for (int k = 0; k < stride; k++) out[np * stride + k] = in[e * stride + k];
            np++;
        }
        if ((da >= 0.0f) != (db >= 0.0f) && np < 4) {
            const float s = da / (da - db);
            for (int k = 0; k < stride; k++)
                out[np * stride + k] = in[e * stride + k] + s * (in[e2 * stride + k] - in[e * stride + k]);
            np++;
        }
    }
    return np;
}
