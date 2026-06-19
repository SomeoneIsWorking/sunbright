#pragma once
// Pure JPA screen-aligned billboard corner math (JPADrawExecBillBoard, JPADrawVisitor.cpp ~L317).
// The particle is a camera-facing quad: its global position is transformed to EYE space (pt), then
// four screen-aligned half-extent offsets are added in eye X/Y at the constant eye Z (pt.z). The
// half-extents come from the per-particle scale (scaleX/Y) times the per-emitter clipboard base size
// (u4) and pivot (uc): x1 = sx·(u4x−ucx), x0 = sx·(u4x+ucx), y0 = sy·(u4y+ucy), y1 = sy·(u4y−ucy);
// corners {(−x0,y0),(x1,y0),(x1,−y1),(−x0,−y1)}. Extracted so the shipping override AND the
// render_test (jpa_billboard) call the SAME function (no forked copy).
//
// Verified against the GX disasm of exec__20JPADrawExecBillBoard @ 0x8033025c.

namespace ngx_jpa {

// out[4][3] = the four eye-space billboard corners.
inline void billboard_corners(float scaleX, float scaleY,
                              float u4x, float u4y, float ucx, float ucy,
                              float ptx, float pty, float ptz,
                              float out[4][3]) {
    const float x1 = scaleX * (u4x - ucx);
    const float x0 = scaleX * (u4x + ucx);
    const float y0 = scaleY * (u4y + ucy);
    const float y1 = scaleY * (u4y - ucy);
    out[0][0] = -x0 + ptx; out[0][1] =  y0 + pty; out[0][2] = ptz;
    out[1][0] =  x1 + ptx; out[1][1] =  y0 + pty; out[1][2] = ptz;
    out[2][0] =  x1 + ptx; out[2][1] = -y1 + pty; out[2][2] = ptz;
    out[3][0] = -x0 + ptx; out[3][1] = -y1 + pty; out[3][2] = ptz;
}

}  // namespace ngx_jpa
