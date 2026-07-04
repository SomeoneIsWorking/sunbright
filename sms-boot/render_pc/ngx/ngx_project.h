#pragma once
// ngx_project — the pure projection unit of the native renderer (extracted from
// ngx_j3d_shape.cpp transform_eye so it can be UNIT-TESTED against spec-computed
// ground truth). eye-space → clip-space → NDC, exactly as GX applies it.
//
// GXSetProjection authors a row-major 4x4. Its row 3 (P[12..15]) encodes the
// projection class WITHOUT a branch here:
//   perspective  row3 = [0, 0, -1, 0]  → w = -ez   (perspective divide)
//   orthographic row3 = [0, 0,  0, 1]  → w =  1     (no foreshortening)
// Applying the FULL 4x4 uniformly is what makes a shape drawn under an ortho
// projection (title-logo models) project flat instead of being foreshortened by
// a stale perspective w — the "dropped ortho" bug this unit pins down.

// clip = P · (eye, 1).  P is row-major 16 floats; clip is 4 floats out.
inline void ngx_project_eye(const float P[16], float ex, float ey, float ez,
                            float clip[4]) {
    clip[0] = P[0]  * ex + P[1]  * ey + P[2]  * ez + P[3];
    clip[1] = P[4]  * ex + P[5]  * ey + P[6]  * ez + P[7];
    clip[2] = P[8]  * ex + P[9]  * ey + P[10] * ez + P[11];
    clip[3] = P[12] * ex + P[13] * ey + P[14] * ez + P[15];
}

// Perspective divide → NDC xy. Returns false when the point is at/behind the eye
// (clip.w <= 0), which the GPU near-clips away — callers must not divide by it.
inline bool ngx_ndc_xy(const float clip[4], float& nx, float& ny) {
    if (clip[3] <= 0.0f) return false;
    nx = clip[0] / clip[3];
    ny = clip[1] / clip[3];
    return true;
}
