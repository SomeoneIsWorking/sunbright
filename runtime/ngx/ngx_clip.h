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
