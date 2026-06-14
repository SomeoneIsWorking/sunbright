// ===========================================================================
// port/pal/mtx.cpp — portable C++ implementations of the GameCube MTX/VEC
// SDK math routines (the PS* paired-single + C_MTX* variants) plus the SMS
// MarioUtil Ms*VEC helpers.
//
// WHY THIS EXISTS
// ---------------
// The decomp implements these in CodeWarrior PowerPC paired-single inline asm
// (psq_l / ps_madd / frsqrte / ...). That asm cannot compile or run on
// x86-64 / arm64, so a link-surface scan of libsmsport_core.a leaves them
// UNDEFINED. This module provides plain portable C++ implementations with the
// EXACT GameCube SDK signatures (so the engine links) and the EXACT numeric
// semantics (so results match the recompiled game).
//
// CONVENTIONS (from reference/sms/include/dolphin/mtx.h):
//   Mtx    = f32[3][4]  — row-major, 3 rows x 4 cols; column 3 is translation.
//   Mtx44  = f32[4][4]  — row-major 4x4.
//   Vec    = { f32 x, y, z }.
// All SDK entry points use C linkage (mtx.h wraps them in extern "C"); the
// MarioUtil Ms* helpers use C++ linkage (declared in MathUtil.hpp, no
// extern "C").
//
// BIT-ACCURACY (critical, see compat/intrinsics.h):
//   The PS* magnitude/normalize routines do NOT use 1/sqrt(x). The Gekko FPU
//   computes a ~12-bit reciprocal-sqrt ESTIMATE (frsqrte) and the asm refines
//   it with a SINGLE Newton-Raphson step. We replicate that exact sequence
//   using the host-accurate __frsqrte estimate from compat/intrinsics.h so the
//   refined result lands on the same bits the game produces. A full-precision
//   sqrt would diverge. The MarioUtil Ms* helpers use the RAW estimate with NO
//   refinement — also replicated exactly.
// ===========================================================================

#include <dolphin/mtx.h>          // exact GC SDK signatures (extern "C")
// compat/intrinsics.h is force-included by the build (-include); it provides
// __frsqrte / __fres / __fsel with the Gekko-accurate estimate tables.

// ---------------------------------------------------------------------------
// Internal helpers (translation unit local).
// ---------------------------------------------------------------------------
namespace {

// Newton-Raphson-refined reciprocal square root, matching the PS asm sequence
//   rsqrt  = frsqrte(s)
//   rsqrt  = rsqrt * 0.5 * (3 - s*rsqrt*rsqrt)
// Used by PSVECNormalize / PSVECMag / PSVECDistance. Returns the refined
// estimate; callers apply the fsel(>=0) zero-guard where the asm does.
inline float ps_rsqrt_refined(float s)
{
    float rsqrt  = (float)__frsqrte((double)s);
    float nwork0 = rsqrt * rsqrt;          // rsqrt^2
    float nwork1 = rsqrt * 0.5f;           // rsqrt/2
    nwork0       = 3.0f - (nwork0 * s);    // fnmsubs: 3 - rsqrt^2 * s
    return nwork0 * nwork1;                // refined 1/sqrt(s)
}

// Branchless floating select: a >= 0 ? b : c  (Gekko fsel semantics).
inline float fsel_(float a, float b, float c) { return (a >= 0.0f) ? b : c; }

} // namespace

// ===========================================================================
// Matrix basics
// ===========================================================================

void PSMTXIdentity(Mtx m)
{
    m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = 0.0f;
    m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f; m[1][3] = 0.0f;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f; m[2][3] = 0.0f;
}

void PSMTXCopy(Mtx src, Mtx dst)
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            dst[i][j] = src[i][j];
}

// mAB = mA . mB, treating each as a 4x4 affine with implicit bottom row
// [0 0 0 1]. Translation (col 3) of mA is added through. Safe for aliasing
// (computes into locals first).
void PSMTXConcat(Mtx mA, Mtx mB, Mtx mAB)
{
    float r[3][4];
    for (int i = 0; i < 3; ++i) {
        r[i][0] = mA[i][0] * mB[0][0] + mA[i][1] * mB[1][0] + mA[i][2] * mB[2][0];
        r[i][1] = mA[i][0] * mB[0][1] + mA[i][1] * mB[1][1] + mA[i][2] * mB[2][1];
        r[i][2] = mA[i][0] * mB[0][2] + mA[i][1] * mB[1][2] + mA[i][2] * mB[2][2];
        r[i][3] = mA[i][0] * mB[0][3] + mA[i][1] * mB[1][3] + mA[i][2] * mB[2][3]
                  + mA[i][3];
    }
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            mAB[i][j] = r[i][j];
}

// Transpose of the 3x3 rotation block; the translation column of the result
// is zeroed (matches the SDK PSMTXTranspose, which builds a pure 3x3 xpose
// padded with a zero translation column).
void PSMTXTranspose(Mtx src, Mtx xPose)
{
    float r[3][4];
    r[0][0] = src[0][0]; r[0][1] = src[1][0]; r[0][2] = src[2][0]; r[0][3] = 0.0f;
    r[1][0] = src[0][1]; r[1][1] = src[1][1]; r[1][2] = src[2][1]; r[1][3] = 0.0f;
    r[2][0] = src[0][2]; r[2][1] = src[1][2]; r[2][2] = src[2][2]; r[2][3] = 0.0f;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            xPose[i][j] = r[i][j];
}

// Inverse of a 3x4 affine: invert the 3x3 rotation/scale block (adjugate/det)
// and set the inverse translation to -(R^-1 . t). Returns 0 if the 3x3 block
// is singular (det == 0) leaving `inv` untouched, else 1. This matches the PS
// asm: it computes the 3x3 cofactors, tests the determinant against zero
// (returns 0 on equal), then scales the cofactor matrix by 1/det and folds in
// the translation column.
u32 PSMTXInverse(Mtx src, Mtx inv)
{
    // 3x3 cofactors (adjugate is the transpose of the cofactor matrix).
    float c00 = src[1][1] * src[2][2] - src[2][1] * src[1][2];
    float c01 = src[1][2] * src[2][0] - src[2][2] * src[1][0];
    float c02 = src[1][0] * src[2][1] - src[2][0] * src[1][1];

    float det = src[0][0] * c00 + src[0][1] * c01 + src[0][2] * c02;
    if (det == 0.0f)
        return 0;

    // The PS asm refines 1/det with a single Newton step off the fres
    // estimate; replicate so the inverse bits match the game.
    float rcp = __fres(det);
    rcp       = rcp + rcp * (1.0f - det * rcp);   // ps_nmsub refinement step

    float c10 = src[0][2] * src[2][1] - src[2][2] * src[0][1];
    float c11 = src[0][0] * src[2][2] - src[2][0] * src[0][2];
    float c12 = src[0][1] * src[2][0] - src[2][1] * src[0][0];
    float c20 = src[0][1] * src[1][2] - src[1][1] * src[0][2];
    float c21 = src[0][2] * src[1][0] - src[1][2] * src[0][0];
    float c22 = src[0][0] * src[1][1] - src[1][0] * src[0][1];

    // inv 3x3 = adjugate / det  (adjugate = transpose of cofactor matrix).
    float i00 = c00 * rcp, i01 = c10 * rcp, i02 = c20 * rcp;
    float i10 = c01 * rcp, i11 = c11 * rcp, i12 = c21 * rcp;
    float i20 = c02 * rcp, i21 = c12 * rcp, i22 = c22 * rcp;

    float tx = src[0][3], ty = src[1][3], tz = src[2][3];

    inv[0][0] = i00; inv[0][1] = i01; inv[0][2] = i02;
    inv[1][0] = i10; inv[1][1] = i11; inv[1][2] = i12;
    inv[2][0] = i20; inv[2][1] = i21; inv[2][2] = i22;

    // inverse translation = -(R^-1 . t)
    inv[0][3] = -(i00 * tx + i01 * ty + i02 * tz);
    inv[1][3] = -(i10 * tx + i11 * ty + i12 * tz);
    inv[2][3] = -(i20 * tx + i21 * ty + i22 * tz);
    return 1;
}

// ===========================================================================
// Matrix builders
// ===========================================================================

void PSMTXTrans(Mtx m, f32 xT, f32 yT, f32 zT)
{
    m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = xT;
    m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f; m[1][3] = yT;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f; m[2][3] = zT;
}

void PSMTXScale(Mtx m, f32 xS, f32 yS, f32 zS)
{
    m[0][0] = xS;   m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = 0.0f;
    m[1][0] = 0.0f; m[1][1] = yS;   m[1][2] = 0.0f; m[1][3] = 0.0f;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = zS;   m[2][3] = 0.0f;
}

// PSMTXRotTrig: rotation about a single axis given sin/cos. Axis is lowercased
// ('x'/'y'/'z'); other values leave m unchanged (matches the asm's fallthrough
// to _end). Builds a pure rotation (zero translation column).
void PSMTXRotTrig(Mtx m, char axis, f32 sinA, f32 cosA)
{
    char a = (char)(axis | 0x20);   // ori axis, 0x20 -> lowercase
    switch (a) {
    case 'x':
        m[0][0] = 1.0f; m[0][1] = 0.0f;  m[0][2] = 0.0f;   m[0][3] = 0.0f;
        m[1][0] = 0.0f; m[1][1] = cosA;  m[1][2] = -sinA;  m[1][3] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = sinA;  m[2][2] = cosA;   m[2][3] = 0.0f;
        break;
    case 'y':
        m[0][0] = cosA;  m[0][1] = 0.0f; m[0][2] = sinA;   m[0][3] = 0.0f;
        m[1][0] = 0.0f;  m[1][1] = 1.0f; m[1][2] = 0.0f;   m[1][3] = 0.0f;
        m[2][0] = -sinA; m[2][1] = 0.0f; m[2][2] = cosA;   m[2][3] = 0.0f;
        break;
    case 'z':
        m[0][0] = cosA; m[0][1] = -sinA; m[0][2] = 0.0f;   m[0][3] = 0.0f;
        m[1][0] = sinA; m[1][1] = cosA;  m[1][2] = 0.0f;   m[1][3] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = 0.0f;  m[2][2] = 1.0f;   m[2][3] = 0.0f;
        break;
    default:
        break;
    }
}

void PSMTXRotRad(Mtx m, char axis, f32 rad)
{
    float s = sinf(rad);
    float c = cosf(rad);
    PSMTXRotTrig(m, axis, s, c);
}

// PSMTXQuat: build a 3x4 rotation matrix from a (possibly unnormalized)
// quaternion (x,y,z,w). Standard quaternion-to-matrix with the SDK's 2/|q|^2
// scale (so an unnormalized quaternion still yields a proper rotation).
// Translation column is zeroed.
void PSMTXQuat(Mtx m, Quaternion* q)
{
    float x = q->x, y = q->y, z = q->z, w = q->w;
    float sq = x * x + y * y + z * z + w * w;
    float s  = (sq != 0.0f) ? (2.0f / sq) : 0.0f;

    float xs = x * s,  ys = y * s,  zs = z * s;
    float wx = w * xs, wy = w * ys, wz = w * zs;
    float xx = x * xs, xy = x * ys, xz = x * zs;
    float yy = y * ys, yz = y * zs, zz = z * zs;

    m[0][0] = 1.0f - (yy + zz); m[0][1] = xy - wz;          m[0][2] = xz + wy;          m[0][3] = 0.0f;
    m[1][0] = xy + wz;          m[1][1] = 1.0f - (xx + zz); m[1][2] = yz - wx;          m[1][3] = 0.0f;
    m[2][0] = xz - wy;          m[2][1] = yz + wx;          m[2][2] = 1.0f - (xx + yy); m[2][3] = 0.0f;
}

// ===========================================================================
// Matrix * vector
// ===========================================================================

// dst = M . (src, 1) — full affine transform (rotation/scale + translation).
void PSMTXMultVec(Mtx44 m, Vec* src, Vec* dst)
{
    float x = src->x, y = src->y, z = src->z;
    float ox = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
    float oy = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
    float oz = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
    dst->x = ox; dst->y = oy; dst->z = oz;
}

// dst = M . src — scale/rotate only (no translation column). "SR" variant.
void PSMTXMultVecSR(Mtx44 m, Vec* src, Vec* dst)
{
    float x = src->x, y = src->y, z = src->z;
    float ox = m[0][0] * x + m[0][1] * y + m[0][2] * z;
    float oy = m[1][0] * x + m[1][1] * y + m[1][2] * z;
    float oz = m[2][0] * x + m[2][1] * y + m[2][2] * z;
    dst->x = ox; dst->y = oy; dst->z = oz;
}

void PSMTXMultVecArray(Mtx m, Vec* srcBase, Vec* dstBase, u32 count)
{
    for (u32 i = 0; i < count; ++i)
        PSMTXMultVec(m, &srcBase[i], &dstBase[i]);
}

// ===========================================================================
// Vector ops (PS* — bit-accurate magnitude/normalize)
// ===========================================================================

void PSVECAdd(Vec* a, Vec* b, Vec* c)
{
    c->x = a->x + b->x; c->y = a->y + b->y; c->z = a->z + b->z;
}

void PSVECSubtract(Vec* a, Vec* b, Vec* c)
{
    c->x = a->x - b->x; c->y = a->y - b->y; c->z = a->z - b->z;
}

void PSVECScale(Vec* src, Vec* dst, f32 scale)
{
    dst->x = src->x * scale; dst->y = src->y * scale; dst->z = src->z * scale;
}

f32 PSVECSquareMag(Vec* v)
{
    return v->x * v->x + v->y * v->y + v->z * v->z;
}

// |v| via frsqrte estimate + one Newton step; fsel-guards |0| -> 0
// (mag = sqsum * rsqrt; the asm guards rsqrt<0 by substituting sqsum, which is
// 0 in the only case it occurs, yielding 0).
float PSVECMag(Vec* v)
{
    float sq    = v->x * v->x + v->y * v->y + v->z * v->z;
    float rsqrt = ps_rsqrt_refined(sq);
    float sel   = fsel_(rsqrt, rsqrt, sq);
    return sq * sel;
}

f32 PSVECDotProduct(Vec* a, Vec* b)
{
    return a->x * b->x + a->y * b->y + a->z * b->z;
}

void PSVECCrossProduct(Vec* a, Vec* b, Vec* dst)
{
    float cx = a->y * b->z - a->z * b->y;
    float cy = a->z * b->x - a->x * b->z;
    float cz = a->x * b->y - a->y * b->x;
    dst->x = cx; dst->y = cy; dst->z = cz;
}

void PSVECNormalize(Vec* vec1, Vec* dst)
{
    float sq    = vec1->x * vec1->x + vec1->y * vec1->y + vec1->z * vec1->z;
    float rsqrt = ps_rsqrt_refined(sq);
    dst->x = vec1->x * rsqrt;
    dst->y = vec1->y * rsqrt;
    dst->z = vec1->z * rsqrt;
}

f32 PSVECSquareDistance(Vec* a, Vec* b)
{
    float dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
    return dx * dx + dy * dy + dz * dz;
}

f32 PSVECDistance(Vec* a, Vec* b)
{
    float dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;
    float sq    = dx * dx + dy * dy + dz * dz;
    float rsqrt = ps_rsqrt_refined(sq);
    float sel   = fsel_(rsqrt, rsqrt, sq);
    return sq * sel;
}

void PSVECHalfAngle(Vec* a, Vec* b, Vec* half)
{
    // negate a and b, sum, normalize (matches C_VECHalfAngle semantics).
    Vec na = { -a->x, -a->y, -a->z };
    Vec nb = { -b->x, -b->y, -b->z };
    Vec sum = { na.x + nb.x, na.y + nb.y, na.z + nb.z };
    float sq = sum.x * sum.x + sum.y * sum.y + sum.z * sum.z;
    if (sq > 0.0f) {
        float rsqrt = ps_rsqrt_refined(sq);
        half->x = sum.x * rsqrt; half->y = sum.y * rsqrt; half->z = sum.z * rsqrt;
    } else {
        half->x = 0.0f; half->y = 0.0f; half->z = 0.0f;
    }
}

void PSVECReflect(Vec* src, Vec* normal, Vec* dst)
{
    // r = src - 2*(src.n)*n, with n normalized first (C_VECReflect semantics).
    Vec n;
    PSVECNormalize(normal, &n);
    float d = src->x * n.x + src->y * n.y + src->z * n.z;
    float k = 2.0f * d;
    dst->x = src->x - k * n.x;
    dst->y = src->y - k * n.y;
    dst->z = src->z - k * n.z;
}

// ===========================================================================
// C_MTX* camera / projection builders (these already had a C reference in the
// decomp; reproduced verbatim so they match exactly).
// ===========================================================================

void C_MTXLookAt(Mtx m, Point3dPtr camPos, VecPtr camUp, Point3dPtr target)
{
    Vec vLook, vRight, vUp;

    vLook.x = camPos->x - target->x;
    vLook.y = camPos->y - target->y;
    vLook.z = camPos->z - target->z;
    PSVECNormalize(&vLook, &vLook);

    PSVECCrossProduct(camUp, &vLook, &vRight);
    PSVECNormalize(&vRight, &vRight);
    PSVECCrossProduct(&vLook, &vRight, &vUp);

    m[0][0] = vRight.x; m[0][1] = vRight.y; m[0][2] = vRight.z;
    m[0][3] = -((camPos->z * vRight.z) + ((camPos->x * vRight.x) + (camPos->y * vRight.y)));

    m[1][0] = vUp.x;    m[1][1] = vUp.y;    m[1][2] = vUp.z;
    m[1][3] = -((camPos->z * vUp.z) + ((camPos->x * vUp.x) + (camPos->y * vUp.y)));

    m[2][0] = vLook.x;  m[2][1] = vLook.y;  m[2][2] = vLook.z;
    m[2][3] = -((camPos->z * vLook.z) + ((camPos->x * vLook.x) + (camPos->y * vLook.y)));
}

void C_MTXFrustum(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f)
{
    f32 tmp;
    tmp     = 1.0f / (r - l);
    m[0][0] = (2 * n) * tmp; m[0][1] = 0.0f; m[0][2] = (r + l) * tmp; m[0][3] = 0.0f;
    tmp     = 1.0f / (t - b);
    m[1][0] = 0.0f; m[1][1] = (2 * n) * tmp; m[1][2] = (t + b) * tmp; m[1][3] = 0.0f;
    m[2][0] = 0.0f; m[2][1] = 0.0f;
    tmp     = 1.0f / (f - n);
    m[2][2] = -(n) * tmp; m[2][3] = -(f * n) * tmp;
    m[3][0] = 0.0f; m[3][1] = 0.0f; m[3][2] = -1.0f; m[3][3] = 0.0f;
}

void C_MTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 n, f32 f)
{
    f32 angle, cot, tmp;
    angle   = (0.5f * fovY);
    angle   = angle * 0.017453293f;
    cot     = 1 / tanf(angle);
    m[0][0] = (cot / aspect); m[0][1] = 0; m[0][2] = 0; m[0][3] = 0;
    m[1][0] = 0; m[1][1] = (cot); m[1][2] = 0; m[1][3] = 0;
    m[2][0] = 0; m[2][1] = 0;
    tmp     = 1 / (f - n);
    m[2][2] = (-n * tmp); m[2][3] = (tmp * -(f * n));
    m[3][0] = 0; m[3][1] = 0; m[3][2] = -1; m[3][3] = 0;
}

void C_MTXOrtho(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f)
{
    f32 tmp;
    tmp     = 1 / (r - l);
    m[0][0] = 2 * tmp; m[0][1] = 0; m[0][2] = 0; m[0][3] = (tmp * -(r + l));
    tmp     = 1 / (t - b);
    m[1][0] = 0; m[1][1] = 2 * tmp; m[1][2] = 0; m[1][3] = (tmp * -(t + b));
    m[2][0] = 0; m[2][1] = 0;
    tmp     = 1 / (f - n);
    m[2][2] = (-1 * tmp); m[2][3] = (-f * tmp);
    m[3][0] = 0; m[3][1] = 0; m[3][2] = 0; m[3][3] = 1;
}

void C_MTXLightFrustum(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 scaleS,
                       f32 scaleT, f32 transS, f32 transT)
{
    f32 _tmp;
    _tmp    = 1 / (r - l);
    m[0][0] = (scaleS * (2 * n * _tmp)); m[0][1] = 0;
    m[0][2] = (scaleS * (_tmp * (r + l))) - transS; m[0][3] = 0;
    _tmp    = 1 / (t - b);
    m[1][0] = 0; m[1][1] = (scaleT * (2 * n * _tmp));
    m[1][2] = (scaleT * (_tmp * (t + b))) - transT; m[1][3] = 0;
    m[2][0] = 0; m[2][1] = 0; m[2][2] = -1; m[2][3] = 0;
}

void C_MTXLightPerspective(Mtx m, f32 fovY, f32 aspect, f32 scaleS, f32 scaleT,
                           f32 transS, f32 transT)
{
    f32 angle, cot;
    angle   = (0.5f * fovY);
    angle   = angle * 0.017453293f;
    cot     = 1 / tanf(angle);
    m[0][0] = (scaleS * (cot / aspect)); m[0][1] = 0; m[0][2] = -transS; m[0][3] = 0;
    m[1][0] = 0; m[1][1] = (cot * scaleT); m[1][2] = -transT; m[1][3] = 0;
    m[2][0] = 0; m[2][1] = 0; m[2][2] = -1; m[2][3] = 0;
}

// ===========================================================================
// MarioUtil Ms* helpers (C++ linkage; declared in MarioUtil/MathUtil.hpp).
//
// MsVECMag2 and MsVECNormalize USED to live here (this TU stood in for the
// deferred MathUtil.cpp while that file still had inline PPC asm). They now
// have their proper home in the owned copy port/src/MathUtil.cpp, which is in
// the core lib (core_sources.txt). Defining them here too would be a duplicate
// symbol → multiple-definition link error, so they were removed. The owned
// MathUtil.cpp implementations are identical (raw frsqrte estimate, no Newton
// refinement, fsel(-sq) zero-guard).
// ===========================================================================
