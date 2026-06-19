#pragma once
#include <cstdint>
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
