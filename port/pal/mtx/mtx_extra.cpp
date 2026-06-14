// ===========================================================================
// port/pal/mtx/mtx_extra.cpp — portable C++ implementations of two GameCube
// MTX SDK math entry points the SMS engine references but that pal/mtx.cpp does
// not yet define: C_MTXLightOrtho and PSMTXScaleApply.
//
// These are REAL math (not bring-up no-ops): they are pure, side-effect-free
// matrix math with well-defined results, ported faithfully from the decomp's
// own reference implementations in reference/sms/src/dolphin/mtx/mtx.c. Kept in
// a SEPARATE file from pal/mtx.cpp (which another track may edit) per the PAL
// task's "new math variants in a new file" guidance.
//
// SIGNATURES: from <dolphin/mtx.h> (extern "C"), matched verbatim. Mtx is
// `f32[3][4]` (row-major, 3 rows x 4 cols; column 3 is translation).
//
//   C_MTXLightOrtho(Mtx m, f32 t,f32 b,f32 l,f32 r, f32 sS,f32 sT, f32 tS,f32 tT)
//     Builds the light-space orthographic texture-projection matrix used for
//     projected-texture / shadow setups. Ported verbatim from mtx.c:547 (the
//     `C functions` reference, not asm) — exact same arithmetic and ordering,
//     so bit-identical to the GC build on the same inputs.
//
//   PSMTXScaleApply(Mtx src, Mtx dst, f32 xS, f32 yS, f32 zS)
//     Scales matrix ROW i by the i-th scale: dst[0][j]=src[0][j]*xS,
//     dst[1][j]=src[1][j]*yS, dst[2][j]=src[2][j]*zS (all 4 columns of each row,
//     translation included). This is the C transliteration of the paired-single
//     asm in mtx.c:390 (psq_l/ps_muls0/psq_st over the 6 pairs). src and dst may
//     alias (the asm loads all rows before storing; we do the same effectively
//     since each output element depends only on the same-index input).
// ===========================================================================

#include <dolphin/mtx.h>

extern "C" {

void C_MTXLightOrtho(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 scaleS, f32 scaleT,
                     f32 transS, f32 transT) {
    f32 tmp;

    tmp     = 1.0f / (r - l);
    m[0][0] = 2.0f * tmp * scaleS;
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = transS + (scaleS * (tmp * -(r + l)));

    tmp     = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = 2.0f * tmp * scaleT;
    m[1][2] = 0.0f;
    m[1][3] = transT + (scaleT * (tmp * -(t + b)));

    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = 0.0f;
    m[2][3] = 1.0f;
}

void PSMTXScaleApply(Mtx src, Mtx dst, f32 xS, f32 yS, f32 zS) {
    // Row 0 * xS, row 1 * yS, row 2 * zS (faithful to the psq_l/ps_muls0/psq_st
    // asm, which scales each row's two column-pairs by the row's scalar).
    dst[0][0] = src[0][0] * xS;
    dst[0][1] = src[0][1] * xS;
    dst[0][2] = src[0][2] * xS;
    dst[0][3] = src[0][3] * xS;
    dst[1][0] = src[1][0] * yS;
    dst[1][1] = src[1][1] * yS;
    dst[1][2] = src[1][2] * yS;
    dst[1][3] = src[1][3] * yS;
    dst[2][0] = src[2][0] * zS;
    dst[2][1] = src[2][1] * zS;
    dst[2][2] = src[2][2] * zS;
    dst[2][3] = src[2][3] * zS;
}

}  // extern "C"
