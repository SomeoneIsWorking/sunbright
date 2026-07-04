#pragma once
#include <cstdint>
#include <cmath>
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

// ── JPA Y-axis billboard (JPADrawExecYBillBoard, JPADrawVisitor.cpp L391) ────────────────────────
// A YBillBoard quad stays vertically upright but tilts in the camera's Y/Z plane (it billboards only
// about the world Y axis). The position is the standard eye-space transform pt = viewMtx·globalPos
// (PNMTX0 = identity here, set by loadYBBMtx). Each local corner offs (z=0) is rotated by the YBB
// matrix unk38 = rows [1,0,0],[0,vy,-vz],[0,vz,vy] with (vy,vz) = normalize(viewMtx[1][1],[2][1]),
// then added to pt. Since offs.z=0 the rotation collapses to corner = (pt.x+offs.x, pt.y+vy·offs.y,
// pt.z+vz·offs.y). vyz = the pre-normalized (viewMtx[1][1], viewMtx[2][1]) pair (caller passes raw;
// this normalizes). Offsets match billboard_corners' layout {(-x0,+y0),(+x1,+y0),(+x1,-y1),(-x0,-y1)}.
inline void jpa_ybillboard_corners(float scaleX, float scaleY,
                                   float u4x, float u4y, float ucx, float ucy,
                                   float ptx, float pty, float ptz,
                                   float vm11, float vm21, float out[4][3]) {
    const float x1 = scaleX * (u4x - ucx);
    const float x0 = scaleX * (u4x + ucx);
    const float y0 = scaleY * (u4y + ucy);
    const float y1 = scaleY * (u4y - ucy);
    float vy = vm11, vz = vm21;
    float n = vy*vy + vz*vz;
    if (n > 1e-12f) { n = 1.0f / sqrtf(n); vy *= n; vz *= n; }   // loadYBBMtx normalizes (0,m11,m21)
    const float ox[4] = { -x0, +x1, +x1, -x0 };
    const float oy[4] = { +y0, +y0, -y1, -y1 };
    for (int i = 0; i < 4; i++) {
        out[i][0] = ptx + ox[i];
        out[i][1] = pty + vy * oy[i];
        out[i][2] = ptz + vz * oy[i];
    }
}

// ── JPA directional particle orientation (JPADrawExecDirectional, JPADrawVisitor.cpp L602) ──────
// A directional particle's quad is oriented in WORLD space by a basis built from the particle's
// stored axis (params.unk0) and a direction vector (velocity / local-pos / emitter-dir, per
// mDirType). Gram-Schmidt: side = unk0 × dir (normalize), then axis = dir × side (normalize); the
// 3×3 column basis is [axis | dir | side]. Local quad offsets (z=0) are rotated by this basis to
// world space; the caller then adds the particle's world position and transforms by the view
// matrix. Returns false if the basis is degenerate (dir≈0 or unk0∥dir) — the game skips that
// particle. dir MUST already be non-zero & normalized by the caller (matches local_BC.normalize()).
inline void cross3(const float a[3], const float b[3], float o[3]) {
    o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0];
}
inline bool norm3(float v[3]) {
    float n=v[0]*v[0]+v[1]*v[1]+v[2]*v[2];
    if (n < 1e-12f) return false;
    n = 1.0f / sqrtf(n); v[0]*=n; v[1]*=n; v[2]*=n; return true;
}
// Build the world orientation basis. unk0/dir are read-only here (the game writes the
// orthonormalised vectors back to the particle; the original guest draw — run first under the
// override — already did that, so we only READ the settled values). outR[r][c]: col0=axis,
// col1=dir, col2=side. Returns false on degeneracy (caller skips the particle, like the game).
inline bool jpa_dir_basis(const float unk0[3], const float dir[3], float outR[3][3]) {
    float side[3]; cross3(unk0, dir, side);
    if (!norm3(side)) return false;
    float axis[3]; cross3(dir, side, axis);
    if (!norm3(axis)) return false;
    outR[0][0]=axis[0]; outR[0][1]=dir[0]; outR[0][2]=side[0];
    outR[1][0]=axis[1]; outR[1][1]=dir[1]; outR[1][2]=side[1];
    outR[2][0]=axis[2]; outR[2][1]=dir[2]; outR[2][2]=side[2];
    return true;
}
// Directional quad corners in WORLD space (offset z=0): out[i] = R·local_offs[i] + pt_world.
// local offsets (JPADrawExecDirectional): x0=-sx(u4x+ucx), y0=+sy(u4y+ucy), x1=+sx(u4x-ucx),
// y1=-sy(u4y-ucy); corners {(x0,y0),(x1,y0),(x1,y1),(x0,y1)}.
inline void jpa_directional_corners(float sx, float sy, float u4x, float u4y, float ucx, float ucy,
                                    const float R[3][3], const float ptw[3], float out[4][3]) {
    const float x0=-sx*(u4x+ucx), y0=+sy*(u4y+ucy), x1=+sx*(u4x-ucx), y1=-sy*(u4y-ucy);
    const float lo[4][2] = {{x0,y0},{x1,y0},{x1,y1},{x0,y1}};
    for (int i=0;i<4;i++) {
        const float lx=lo[i][0], ly=lo[i][1];   // local z = 0
        out[i][0] = R[0][0]*lx + R[0][1]*ly + ptw[0];
        out[i][1] = R[1][0]*lx + R[1][1]*ly + ptw[1];
        out[i][2] = R[2][0]*lx + R[2][1]*ly + ptw[2];
    }
}
// DirBillBoard (JPADrawExecDirBillBoard, L918): a SCREEN-aligned quad rotated in the view plane to
// follow the particle's direction. The eye-space direction is (ex,ey) = the normalized
// (dir × cameraUp) rotated into eye space; the 2D offsets are rotated by the complex factor (ex+i·ey).
// Offsets here: x0=-sx(u4x-ucx), y0=+sy(u4y-ucy), x1=+sx(u4x+ucx), y1=-sy(u4y+ucy).
inline void jpa_dirbb_offsets(float sx, float sy, float u4x, float u4y, float ucx, float ucy,
                              float ex, float ey, float out[4][2]) {
    const float x0=-sx*(u4x-ucx), y0=+sy*(u4y-ucy), x1=+sx*(u4x+ucx), y1=-sy*(u4y+ucy);
    const float o[4][2] = {{x0,y0},{x1,y0},{x1,y1},{x0,y1}};
    for (int i=0;i<4;i++) { out[i][0]=ex*o[i][0]-ey*o[i][1]; out[i][1]=ex*o[i][1]+ey*o[i][0]; }
}

// ── JPA stripe/ribbon edge math (JPADrawExecStripe / StripeCross, JPADrawVisitor.cpp L1117/L1193) ──
// A stripe is an EMITTER-level visitor: it connects ALL the emitter's particles into one continuous
// ribbon (a GX_TRIANGLESTRIP, not per-particle quads). For each particle it computes TWO edge points
// (the left/right rails of the ribbon at that particle) and connects consecutive particles into
// segment quads. The two rails come from the particle's width (params.unk10), rotation (params.unk34
// → sin/cos), and the orientation basis built from the particle's stored axis (params.unk0, settled)
// and a direction vector (velocity / local-pos / …, per mDirType). The basis is the SAME Gram-Schmidt
// as the directional billboard but the column ORDER differs: here M = [axis | side | dir] (the decomp
// rows u1=(unk0.x,side.x,dir.x) etc → those are M's ROWS, so the columns are axis,side,dir). The two
// rail offsets are v1=(x·sin, x·cos, 0) and v2=(y·sin, y·cos, 0) with x=−w(u4x+ucx), y=+w(u4x−ucx);
// each rail = pt0 + M·v. Extracted so the override + render_test (jpa_stripe) share the function.
inline void jpa_stripe_basis(const float axis_in[3], const float dir_in[3], float M[3][3]) {
    float bc[3] = {dir_in[0], dir_in[1], dir_in[2]};
    if (!norm3(bc)) { bc[0]=0; bc[1]=1; bc[2]=0; }          // local_BC.isZero() → (0,1,0)
    float side[3]; cross3(axis_in, bc, side);               // f29 = unk0 × local_BC
    if (!norm3(side)) { side[0]=0; side[1]=1; side[2]=0; }
    float axis[3]; cross3(bc, side, axis); norm3(axis);     // unk0 = local_BC × f29 (re-derive; settled)
    M[0][0]=axis[0]; M[0][1]=side[0]; M[0][2]=bc[0];        // cols: [axis | side | dir]
    M[1][0]=axis[1]; M[1][1]=side[1]; M[1][2]=bc[1];
    M[2][0]=axis[2]; M[2][1]=side[2]; M[2][2]=bc[2];
}
// Compute one particle's two ribbon rail points (world space). width = params.unk10 (primary ribbon)
// or params.unk14 (StripeCross's perpendicular 2nd ribbon). sinA/cosA = JMASSin/Cos(unk34) (the 2nd
// cross ribbon passes sin=-JMASSin, cos=JMASCos). axis_in = params.unk0 (read-only, settled). dir_in
// = the (un-normalized) direction vector; normalized here with the game's zero→(0,1,0) fallback.
inline void jpa_stripe_corners(float width, float u4x, float ucx, float sinA, float cosA,
                               const float axis_in[3], const float dir_in[3], const float pt0[3],
                               float left[3], float right[3]) {
    float M[3][3]; jpa_stripe_basis(axis_in, dir_in, M);
    const float x = -width * (u4x + ucx);
    const float y = +width * (u4x - ucx);
    const float v1[3] = {x*sinA, x*cosA, 0.f};
    const float v2[3] = {y*sinA, y*cosA, 0.f};
    for (int r = 0; r < 3; r++) {
        left[r]  = pt0[r] + M[r][0]*v1[0] + M[r][1]*v1[1] + M[r][2]*v1[2];
        right[r] = pt0[r] + M[r][0]*v2[0] + M[r][1]*v2[1] + M[r][2]*v2[2];
    }
}

// ── JPA TEV combiner encoding (JPADrawSetupTev::setupTev, JPADrawSetupTev.cpp:13-19) ─────
// The single-stage particle combiner. The COLOUR inputs (a,b,c,d) are the GX_CC_* selectors
// the loader baked from JPABaseShape data[0x30] (the "colour input type"); the four cases:
//   0: ZERO,TEXC,ONE,ZERO  → out = TEXC                (pure texture)
//   1: ZERO,C0,TEXC,ZERO   → out = C0·TEXC             (texture × prm — the common MODULATE)
//   2: C0,ONE,TEXC,ZERO    → lerp(C0, white, TEXC)
//   3: C1,C0,TEXC,ZERO     → lerp(C1, C0, TEXC)
//   4: ZERO,TEXC,C0,C1     → TEXC·C0 + C1
//   5: ZERO,ZERO,ZERO,C0   → out = C0                  (flat prm, no texture)
// op=GX_TEV_ADD(0), bias=GX_TB_ZERO(0), scale=GX_CS_SCALE_1(0), clamp=true, dest=GX_TEVPREV(0).
// The packed 24-bit register the shader generator decodes: a@12-15, b@8-11, c@4-7, d@0-3,
// bias@16-17, op@18, clamp@19, scale@20-21, dest@22-23 (decode_cc in tev_shader.cpp).
inline uint32_t jpa_color_env(int a, int b, int c, int d) {
    return ((uint32_t)(a & 0xf) << 12) | ((uint32_t)(b & 0xf) << 8) |
           ((uint32_t)(c & 0xf) << 4) | (uint32_t)(d & 0xf) | (1u << 19);   // clamp
}
// Fixed JPA alpha combiner: GXSetTevAlphaIn(ZERO,TEXA,A0,ZERO) → out = TEXA·A0 (tex alpha × prm
// alpha register). a@13-15, b@10-12, c@7-9, d@4-6 (3-bit GX_CA_*); rswap/tswap (bits 0-3) = 0
// (identity swap table). GX_CA_ZERO=7, GX_CA_TEXA=4, GX_CA_A0=1.
inline uint32_t jpa_alpha_env() {
    return (7u << 13) | (4u << 10) | (1u << 7) | (7u << 4) | (1u << 19);
}

// One particle's renderer-ready quad: eye-space corners + GX texcoords + the resolved TEV combiner,
// TEV colour registers (C0=prm, C1=env), texmap0 binding, and PE state (blend/zmode). Built by
// jpa_particle_native.cpp, consumed by ngx_emit_particle_quad (ngx_j3d_shape.cpp).
struct NgxParticleQuad {
    float    eye[4][3];        // billboard corners in eye/view space
    float    uv[4][2];         // GX_TEXCOORD0 per corner (cb.mTexCoords)
    uint32_t color_env;        // stage0 colour combiner (jpa_color_env)
    uint32_t alpha_env;        // stage0 alpha  combiner (jpa_alpha_env)
    int16_t  c0[4];            // GX_TEVREG0 (C0/A0) = prm colour, S10 RGBA (0..255)
    int16_t  c1[4];            // GX_TEVREG1 (C1)    = env colour, S10 RGBA
    uint32_t tex_addr;         // texmap0 tiled image (0 = none → 1×1 white)
    uint32_t tlut_addr;        // CI palette (0 = none)
    uint16_t tex_w, tex_h;
    uint8_t  tex_fmt, tlut_fmt;
    uint8_t  wrap_s, wrap_t, min_filter, mag_filter;
    uint8_t  blend_mode, src_factor, dst_factor;   // GXSetBlendMode (live, per shape)
    uint8_t  z_test, z_write, z_func;              // GXSetZMode (live)
};

}  // namespace ngx_jpa
