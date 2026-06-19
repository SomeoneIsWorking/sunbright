#pragma once
// ngx_indirect — the PURE (Dolphin-free, testable) pieces of the GX indirect-texturing
// port. The texcoord-warp math itself lives in the generated GLSL (tev_shader.cpp), like
// the TEV combiner — but the bits that are plain integer arithmetic done at capture time
// (the IndTexMtx float→S2.10-mantissa quantization, and the GXIndTexMtxID decode) are
// here so render_test can lock them against hand-computed GX values.
//
// Spec: reference/sms GXBump.c GXSetIndTexMtx + externals/dolphin PixelShaderManager
// SetIndMatrixChanged. See debug_journal/2026-06-19_indirect_texturing_re.md.
#include <cstdint>
#include <cmath>

namespace ngx {

// GXSetIndTexMtx stores each offset-matrix element as an 11-bit signed S2.10 mantissa:
//   mtx = (int)(1024.0f * offset) & 0x7FF
// i.e. round-toward-zero × 1024, keep low 11 bits, interpreted as signed 11-bit. The
// shader reads these back as the signed mantissa (Dolphin: bpmem.indmtx[].col0.ma, an
// 11-bit signed field). Returns the sign-extended value in [-1024, 1023].
inline int16_t ngx_ind_mtx_mantissa(float offset) {
    int v = (int)(1024.0f * offset);   // C cast truncates toward zero, matching GXBump.c
    v &= 0x7FF;                        // keep 11 bits
    if (v & 0x400) v -= 0x800;         // sign-extend bit 10
    return (int16_t)v;
}

// Decode a GXIndTexMtxID (J3DIndTevStage::mMtxSel) into the 0-based indirect-matrix index
// [0..2] and a kind. Returns false for GX_ITM_OFF (0) — indirect disabled for the stage.
//   1-3  = GX_ITM_0..2   → kind 0 (Indirect: full 2x3 offset matrix)
//   5-7  = GX_ITM_S0..2  → kind 1 (S: scale the regular S coord by the indirect S value)
//   9-11 = GX_ITM_T0..2  → kind 2 (T: scale the regular T coord by the indirect T value)
enum { NGX_ITM_INDIRECT = 0, NGX_ITM_S = 1, NGX_ITM_T = 2 };
inline bool ngx_ind_mtx_decode(uint8_t mtx_sel, int* mtx_idx, int* kind) {
    if (mtx_sel == 0) return false;                        // GX_ITM_OFF
    if (mtx_sel >= 1 && mtx_sel <= 3)  { *mtx_idx = mtx_sel - 1; *kind = NGX_ITM_INDIRECT; return true; }
    if (mtx_sel >= 5 && mtx_sel <= 7)  { *mtx_idx = mtx_sel - 5; *kind = NGX_ITM_S;        return true; }
    if (mtx_sel >= 9 && mtx_sel <= 11) { *mtx_idx = mtx_sel - 9; *kind = NGX_ITM_T;        return true; }
    return false;                                          // reserved / invalid
}

// Format-dependent right shift applied to the raw indirect texel before use as a coord
// (GX_ITF_8/5/4/3 → 0/3/4/5), and the bias added per selected component (ITF_8 → -128,
// else +1). Both straight from PixelShaderGen (tev_ind_fmt_shift / tev_ind_bias_add).
inline int ngx_ind_fmt_shift(uint8_t fmt) {
    static const int s[4] = { 0, 3, 4, 5 };
    return s[fmt & 3];
}
inline int ngx_ind_bias_add(uint8_t fmt) { return (fmt & 3) == 0 ? -128 : 1; }

}  // namespace ngx
