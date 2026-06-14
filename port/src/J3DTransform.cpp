// Owned portable copy of
// reference/sms/src/JSystem/J3D/J3DGraphBase/J3DTransform.cpp — the
// paired-single inline-PPC-asm function bodies are reimplemented in portable
// C++ with identical matrix/vector semantics. Keep in sync with the decomp.
//
// Matrix conventions (dolphin/mtx.h):
//   Mtx     = f32[3][4]      row-major 3x4 (col 3 = translation); MtxPtr=(*)[4]
//   ROMtx   = f32[4][3]      packed 3x3 stored as rows of 3;      ROMtxPtr=(*)[3]
//   Mtx44   = f32[4][4]
// Each reimplemented routine carries a note mapping the asm to its C++ form;
// the paired-single (psq_l/ps_mul/ps_madd/...) ops were decoded by hand and the
// math verified. The non-asm functions below are verbatim from the decomp.

#include <JSystem/J3D/J3DGraphBase/J3DTransform.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DStruct.hpp>
#include <JSystem/JMath.hpp>

J3DTransformInfo const j3dDefaultTransformInfo
    = { { 1.0f, 1.0f, 1.0f }, { 0, 0, 0 }, { 0.0f, 0.0f, 0.0f } };

// clang-format off
Mtx const j3dDefaultMtx = {
	1.0f, 0.0f, 0.0f, 0.0f,
	0.0f, 1.0f, 0.0f, 0.0f,
	0.0f, 0.0f, 1.0f, 0.0f,
	};
// clang-format on

f32 PSMulUnit01[2] = { 0.0f, -1.0f };

f32 Unit01[2] = { 0.0f, 1.0f };

#define qr0 0

#pragma push
#pragma fp_contract off
// TODO: several other functions in this TU use fp_contract,
// but this one for some reason doesn't?!
f32 J3DCalcZValue(MtxPtr m, Vec v)
{
	/* Nonmatching */
	return m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3];
}
#pragma pop

// Inverse-transpose of the 3x3 rotation/scale block of `src` (a 3x4 Mtx),
// stored into `dst` (a packed 3x3 ROMtx). Returns false if singular (det==0)
// leaving dst untouched, else true. Used to build the normal-transform matrix.
//
// Decoded asm (aij = src[i][j], the 3x3 block; translation col ignored):
//   result[r][c] = cofactor(M)[r][c] / det   (== (M^-1)^T, the inverse-xpose)
// The asm computes the 3x3 cofactor matrix, the determinant via cofactor
// expansion, rejects det==0 (ps_cmpo0 / bne), then refines 1/det with the fres
// estimate + TWO ps_nmsub Newton steps (matches the game's bits — NOT 1.0/det),
// and scales each cofactor by 1/det. The store layout writes a standard 3x3 in
// ROMtx (rows of 3) order, so dst[r][c] is the (r,c) cofactor / det.
bool J3DPSCalcInverseTranspose(register MtxPtr src, register ROMtxPtr dst)
{
	const f32 a00 = src[0][0], a01 = src[0][1], a02 = src[0][2];
	const f32 a10 = src[1][0], a11 = src[1][1], a12 = src[1][2];
	const f32 a20 = src[2][0], a21 = src[2][1], a22 = src[2][2];

	// Cofactor matrix (signed minors), exactly as the paired-single ops build.
	const f32 c00 = a11 * a22 - a21 * a12;
	const f32 c01 = a12 * a20 - a22 * a10;
	const f32 c02 = a10 * a21 - a11 * a20;
	const f32 c10 = a21 * a02 - a01 * a22;
	const f32 c11 = a22 * a00 - a02 * a20;
	const f32 c12 = a01 * a20 - a00 * a21;
	const f32 c20 = a01 * a12 - a11 * a02;
	const f32 c21 = a02 * a10 - a12 * a00;
	const f32 c22 = a00 * a11 - a01 * a10;

	// Determinant by cofactor expansion along column 0 (matches the asm's
	// f7 = a00*c00 + a10*c10' + a20*c20' accumulation).
	const f32 det = a00 * c00 + a10 * c10 + a20 * c20;
	if (det == 0.0f) // ps_cmpo0 cr0,det,0 ; bne skip ; else return false
		return false;

	// 1/det: fres estimate + two Newton-Raphson refinement steps (ps_nmsub),
	// replicating the asm so the inverse bits match the recompiled game.
	f32 r = __fres(det);
	r = r + r * (1.0f - det * r); // ps_add/ps_mul/ps_nmsub step 1
	r = r + r * (1.0f - det * r); // step 2

	dst[0][0] = c00 * r; dst[0][1] = c01 * r; dst[0][2] = c02 * r;
	dst[1][0] = c10 * r; dst[1][1] = c11 * r; dst[1][2] = c12 * r;
	dst[2][0] = c20 * r; dst[2][1] = c21 * r; dst[2][2] = c22 * r;
	return true;
}

void J3DGetTranslateRotateMtx(const J3DTransformInfo& tx, Mtx dst)
{
	f32 sx = JMASSin(tx.mRotation.x), cx = JMASCos(tx.mRotation.x);
	f32 sy = JMASSin(tx.mRotation.y), cy = JMASCos(tx.mRotation.y);
	f32 sz = JMASSin(tx.mRotation.z), cz = JMASCos(tx.mRotation.z);

	dst[2][0] = -sy;
	dst[0][0] = cz * cy;
	dst[1][0] = sz * cy;
	dst[2][1] = cy * sx;
	dst[2][2] = cy * cx;

	f32 cxsz  = cx * sz;
	f32 sxcz  = sx * cz;
	dst[0][1] = sxcz * sy - cxsz;
	dst[1][2] = cxsz * sy - sxcz;

	f32 sxsz  = sx * sz;
	f32 cxcz  = cx * cz;
	dst[0][2] = cxcz * sy + sxsz;
	dst[1][1] = sxsz * sy + cxcz;

	dst[0][3] = tx.mTranslate.x;
	dst[1][3] = tx.mTranslate.y;
	dst[2][3] = tx.mTranslate.z;
}

void J3DGetTranslateRotateMtx(s16 rx, s16 ry, s16 rz, f32 tx, f32 ty, f32 tz,
                              Mtx dst)
{
	f32 sx = JMASSin(rx), cx = JMASCos(rx);
	f32 sy = JMASSin(ry), cy = JMASCos(ry);
	f32 sz = JMASSin(rz), cz = JMASCos(rz);

	dst[2][0] = -sy;
	dst[0][0] = cz * cy;
	dst[1][0] = sz * cy;
	dst[2][1] = cy * sx;
	dst[2][2] = cy * cx;

	f32 cxsz  = cx * sz;
	f32 sxcz  = sx * cz;
	dst[0][1] = sxcz * sy - cxsz;
	dst[1][2] = cxsz * sy - sxcz;

	f32 sxsz  = sx * sz;
	f32 cxcz  = cx * cz;
	dst[0][2] = cxcz * sy + sxsz;
	dst[1][1] = sxsz * sy + cxcz;

	dst[0][3] = tx;
	dst[1][3] = ty;
	dst[2][3] = tz;
}

void J3DGetTextureMtx(const J3DTextureSRTInfo& srt, Vec center, Mtx dst)
{
	f32 sr = JMASSin(srt.mRotation), cr = JMASCos(srt.mRotation);

	dst[0][0] = srt.mScaleX * cr;
	dst[0][1] = -srt.mScaleX * sr;
	dst[0][2] = (-srt.mScaleX * cr * center.x + srt.mScaleX * sr * center.y)
	            + center.x + srt.mTranslationX;

	dst[1][0] = srt.mScaleY * sr;
	dst[1][1] = srt.mScaleY * cr;
	dst[1][2] = (-srt.mScaleY * sr * center.x - srt.mScaleY * cr * center.y)
	            + center.y + srt.mTranslationY;

	dst[2][3] = 0.0f;
	dst[2][1] = 0.0f;
	dst[2][0] = 0.0f;
	dst[1][3] = 0.0f;
	dst[0][3] = 0.0f;
	dst[2][2] = 1.0f;
}

void J3DGetTextureMtxOld(const J3DTextureSRTInfo& srt, Vec center, Mtx dst)
{
	f32 sr = JMASSin(srt.mRotation), cr = JMASCos(srt.mRotation);

	dst[0][0] = srt.mScaleX * cr;
	dst[0][1] = -srt.mScaleX * sr;
	dst[0][3] = (-srt.mScaleX * cr * center.x + srt.mScaleX * sr * center.y)
	            + center.x + srt.mTranslationX;

	dst[1][0] = srt.mScaleY * sr;
	dst[1][1] = srt.mScaleY * cr;
	dst[1][3] = (-srt.mScaleY * sr * center.x - srt.mScaleY * cr * center.y)
	            + center.y + srt.mTranslationY;

	dst[2][3] = 0.0f;
	dst[2][1] = 0.0f;
	dst[2][0] = 0.0f;
	dst[1][2] = 0.0f;
	dst[0][2] = 0.0f;
	dst[2][2] = 1.0f;
}

void J3DGetTextureMtxMaya(const J3DTextureSRTInfo& srt, MtxPtr dst)
{
	dst[0][0] = srt.mScaleX * JMASCos(srt.mRotation);
	dst[0][1] = srt.mScaleY * JMASSin(srt.mRotation);
	dst[0][2]
	    = (srt.mTranslationX - 0.5f) * JMASCos(srt.mRotation)
	      - JMASSin(srt.mRotation) * ((srt.mTranslationY - 0.5f) + srt.mScaleY)
	      + 0.5f;
	dst[0][3] = 0.0f;

	dst[1][0] = -srt.mScaleX * JMASSin(srt.mRotation);
	dst[1][1] = srt.mScaleY * JMASCos(srt.mRotation);
	dst[1][2]
	    = -(srt.mTranslationX - 0.5f) * JMASSin(srt.mRotation)
	      - JMASCos(srt.mRotation) * ((srt.mTranslationY - 0.5f) + srt.mScaleY)
	      + 0.5f;
	dst[1][3] = 0.0f;

	dst[2][0] = 0.0f;
	dst[2][1] = 0.0f;
	dst[2][2] = 1.0f;
	dst[2][3] = 0.0f;
}

void J3DGetTextureMtxMayaOld(const J3DTextureSRTInfo& srt, Mtx dst)
{
	dst[0][0] = srt.mScaleX * JMASCos(srt.mRotation);
	dst[0][1] = srt.mScaleY * JMASSin(srt.mRotation);
	dst[0][2] = 0.0f;
	dst[0][3]
	    = (srt.mTranslationX - 0.5f) * JMASCos(srt.mRotation)
	      - JMASSin(srt.mRotation) * ((srt.mTranslationY - 0.5f) + srt.mScaleY)
	      + 0.5f;

	dst[1][0] = -srt.mScaleX * JMASSin(srt.mRotation);
	dst[1][1] = srt.mScaleY * JMASCos(srt.mRotation);
	dst[1][2] = 0.0f;
	dst[1][3]
	    = -(srt.mTranslationX - 0.5f) * JMASSin(srt.mRotation)
	      - JMASCos(srt.mRotation) * ((srt.mTranslationY - 0.5f) + srt.mScaleY)
	      + 0.5f;

	dst[2][0] = 0.0f;
	dst[2][1] = 0.0f;
	dst[2][2] = 1.0f;
	dst[2][3] = 0.0f;
}

// Scale the columns of a packed 3x3 (ROMtx) in place by (scl.x, scl.y, scl.z):
//   mtx[i][0] *= scl.x; mtx[i][1] *= scl.y; mtx[i][2] *= scl.z   for i=0..2.
// The asm loads each row's {col0,col1} as a paired-single and multiplies by
// {scl.x, scl.y}, and col2 by scl.z (offsets 0/8, 12/20, 24/32 = ROMtx rows).
void J3DScaleNrmMtx33(register ROMtxPtr mtx, const register Vec& scl)
{
	for (int i = 0; i < 3; ++i) {
		mtx[i][0] *= scl.x;
		mtx[i][1] *= scl.y;
		mtx[i][2] *= scl.z;
	}
}

// Projection concat: result = param_1 (3x4) . param_2 (treated as a full 4x4),
//   result[r][c] = sum_{k=0..3} param_1[r][k] * param_2[k][c]   (r=0..2, c=0..3)
// Unlike PSMTXConcat (which adds param_1's translation column through an
// implicit [0 0 0 1] bottom row), this multiplies param_1's row by ALL FOUR
// rows of param_2 — i.e. param_2's 4th row participates. Safe for aliasing
// (computes into a local first). The asm broadcasts each param_1[r][k] and
// ps_madds it against param_2's rows two columns at a time.
void J3DMtxProjConcat(register Mtx param_1, register Mtx param_2,
                      register Mtx result)
{
	f32 r[3][4];
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 4; ++j) {
			r[i][j] = param_1[i][0] * param_2[0][j]
			          + param_1[i][1] * param_2[1][j]
			          + param_1[i][2] * param_2[2][j]
			          + param_1[i][3] * param_2[3][j];
		}
	}
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 4; ++j)
			result[i][j] = r[i][j];
}

// Copy a packed 3x3 (ROMtx, 9 contiguous floats) from src to dst.
// The asm streams 32 bytes as four paired-singles + one single (offsets
// 0,8,16,24,32), which is exactly the 9 floats of the 3x3 in ROMtx order.
void J3DPSMtx33Copy(register ROMtxPtr src, register ROMtxPtr dst)
{
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			dst[i][j] = src[i][j];
}

// Copy the 3x3 rotation/scale block of a 3x4 Mtx (`src`, MtxPtr) into a packed
// 3x3 ROMtx (`dst`), dropping the translation column. The asm reads src rows at
// 0/8 (cols 0,1 / col 2), 16/24, 32/40 and writes them packed at dst 0/8,
// 12/20, 24/32 (ROMtx rows of 3): dst[i][j] = src[i][j] for i,j in 0..2.
void J3DPSMtx33CopyFrom34(register MtxPtr src, register ROMtxPtr dst)
{
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			dst[i][j] = src[i][j];
}

// Copy an array of `size` consecutive 3x4 matrices (each 12 floats / 48 bytes)
// from src to dst. The asm streams 6 paired-singles per matrix (the pre-decrement
// by 8 + psq_lu/stu at 0x30 is just the loop's auto-increment of one Mtx stride).
void J3DPSMtxArrayCopy(register MtxPtr src, register MtxPtr dst,
                       register u32 size)
{
	for (u32 n = 0; n < size; ++n)
		for (int j = 0; j < 4; ++j) {
			dst[3 * n + 0][j] = src[3 * n + 0][j];
			dst[3 * n + 1][j] = src[3 * n + 1][j];
			dst[3 * n + 2][j] = src[3 * n + 2][j];
		}
}

void J3DMTXConcatArrayIndexedSrc(register const float (*mat1)[4],
                                 register const float (*mat2)[3][4],
                                 register const u16* param_3, register Mtx* dst,
                                 register u32 count)
{
	// dst[i] = mat1 . mat2[indices[i]]   for i = 0..count-1.
	// `mat1` is one 3x4 Mtx; `mat2` is an array of 3x4 Mtx (the index scales by
	// 0x30 = sizeof(Mtx)); `param_3` is the u16 index stream. The concat is the
	// standard affine product: result[r][c] = sum_k mat1[r][k]*mat2idx[k][c]
	// for c=0..2, and for c==3 the same sum PLUS mat1[r][3] (mat1's translation
	// passes through the implicit [0 0 0 1] row — the asm's Unit01={0,1} +
	// ps_madd that adds mat1[r][3]). Equivalent to PSMTXConcat(mat1, mat2idx).
	for (u32 i = 0; i < count; ++i) {
		const float (*m2)[4] = mat2[param_3[i]];
		float (*out)[4]      = dst[i];
		for (int r = 0; r < 3; ++r) {
			out[r][0] = mat1[r][0] * m2[0][0] + mat1[r][1] * m2[1][0]
			            + mat1[r][2] * m2[2][0];
			out[r][1] = mat1[r][0] * m2[0][1] + mat1[r][1] * m2[1][1]
			            + mat1[r][2] * m2[2][1];
			out[r][2] = mat1[r][0] * m2[0][2] + mat1[r][1] * m2[1][2]
			            + mat1[r][2] * m2[2][2];
			out[r][3] = mat1[r][0] * m2[0][3] + mat1[r][1] * m2[1][3]
			            + mat1[r][2] * m2[2][3] + mat1[r][3];
		}
	}
}

// Concatenate the single left matrix `fst` with each of `size` right matrices
// in the `snd` array, writing dst[i] = fst . snd[i]  (i = 0..size-1).
// `fst` stays fixed across the loop; `snd` and `dst` advance by one Mtx each
// iteration (the asm's subi/psq_*u). Standard affine concat (same as
// PSMTXConcat / J3DMTXConcatArrayIndexedSrc): for c=0..2 it's the plain row.col
// product; for c==3 it also adds fst[r][3] (fst's translation through the
// implicit [0 0 0 1] row — the asm's Unit01={0,1} + ps_madds1 term).
void J3DPSMtxArrayConcat(register Mtx fst, register Mtx snd,
                         register Mtx dst, register u32 size)
{
	for (u32 i = 0; i < size; ++i) {
		const float (*B)[4] = &snd[3 * i]; // snd[i] (3 rows)
		float (*out)[4]     = &dst[3 * i]; // dst[i]
		for (int r = 0; r < 3; ++r) {
			out[r][0] = fst[r][0] * B[0][0] + fst[r][1] * B[1][0]
			            + fst[r][2] * B[2][0];
			out[r][1] = fst[r][0] * B[0][1] + fst[r][1] * B[1][1]
			            + fst[r][2] * B[2][1];
			out[r][2] = fst[r][0] * B[0][2] + fst[r][1] * B[1][2]
			            + fst[r][2] * B[2][2];
			out[r][3] = fst[r][0] * B[0][3] + fst[r][1] * B[1][3]
			            + fst[r][2] * B[2][3] + fst[r][3];
		}
	}
}
