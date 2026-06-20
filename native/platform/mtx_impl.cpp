// mtx_impl.cpp — native PC implementation of the GC MTX/VEC math seam.
//
// Implements the SDK symbols the game actually links (non-DEBUG dispatch maps the
// public MTX*/VEC* macros to PSMTX*/PSVEC*; a few C_MTX* are called directly).
// Grounded in the unresolved-symbol set of libsms-native.a (see scratch/native_unresolved.txt).
//
// FIDELITY NOTE: the GC paired-single (PS*) routines and their C_* portable
// counterparts are mathematically identical — the SDK ships C_MTXConcat as the
// documented reference for PSMTXConcat. Where the decomp has a C_* body (mtx.c /
// mtxvec.c / vec.c / mtx44.c) we port it verbatim; the asm-only ops are implemented
// from standard, well-defined GC MTX semantics (row-major affine Mtx[3][4],
// Mtx44[4][4]) and pinned by the unit tests in mtx_test.cpp.
//
// RUNTIME LANDMINE (documented, accepted for the native engine): PSVECNormalize/
// PSVECMag/PSVECDistance and PSMTXInverse on HW use the Gekko frsqrte/fres ~12-bit
// estimate + one Newton-Raphson step. We use EXACT host sqrtf/division (float-exact,
// strictly MORE precise; one NR step from a 12-bit estimate is already ~float-exact).
// If a future divergence vs the Dolphin oracle ever traces to normalize/inverse
// precision, route through native/shim/gekko_intrinsics.h (__frsqrte/__fres) instead.

#include <dolphin/mtx.h>
#include <cmath>

extern "C" {

// ===========================================================================
// MTX — 3x4 affine matrices, row-major (Mtx[row][col]).
// ===========================================================================

void PSMTXIdentity(Mtx m) {
    m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = 0.0f;
    m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f; m[1][3] = 0.0f;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f; m[2][3] = 0.0f;
}

void PSMTXCopy(Mtx src, Mtx dst) {
    if (src == dst) return;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) dst[i][j] = src[i][j];
}

// ab = a * b, treating each as a 4x4 with implicit bottom row [0 0 0 1].
// Buffers into a local so aliasing (ab == a or ab == b) is safe.
void PSMTXConcat(Mtx a, Mtx b, Mtx ab) {
    Mtx t;
    for (int i = 0; i < 3; ++i) {
        t[i][0] = a[i][0]*b[0][0] + a[i][1]*b[1][0] + a[i][2]*b[2][0];
        t[i][1] = a[i][0]*b[0][1] + a[i][1]*b[1][1] + a[i][2]*b[2][1];
        t[i][2] = a[i][0]*b[0][2] + a[i][1]*b[1][2] + a[i][2]*b[2][2];
        t[i][3] = a[i][0]*b[0][3] + a[i][1]*b[1][3] + a[i][2]*b[2][3] + a[i][3];
    }
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) ab[i][j] = t[i][j];
}

void PSMTXTranspose(Mtx src, Mtx xPose) {
    // Transpose the 3x3 rotation part; translation column becomes 0
    // (C_MTXTranspose semantics).
    Mtx t;
    t[0][0] = src[0][0]; t[0][1] = src[1][0]; t[0][2] = src[2][0]; t[0][3] = 0.0f;
    t[1][0] = src[0][1]; t[1][1] = src[1][1]; t[1][2] = src[2][1]; t[1][3] = 0.0f;
    t[2][0] = src[0][2]; t[2][1] = src[1][2]; t[2][2] = src[2][2]; t[2][3] = 0.0f;
    PSMTXCopy(t, xPose);
}

// Inverse of a 3x4 affine matrix. Returns 0 (singular) if the 3x3 determinant is
// exactly 0, else 1 — matching the SDK C_MTXInverse contract.
u32 PSMTXInverse(Mtx src, Mtx inv) {
    f32 det = src[0][0] * (src[1][1]*src[2][2] - src[2][1]*src[1][2])
            - src[1][0] * (src[0][1]*src[2][2] - src[2][1]*src[0][2])
            + src[2][0] * (src[0][1]*src[1][2] - src[1][1]*src[0][2]);
    if (det == 0.0f) return 0;
    f32 idet = 1.0f / det;

    Mtx t;
    // Cofactor / adjugate of the 3x3, scaled by 1/det.
    t[0][0] =  (src[1][1]*src[2][2] - src[2][1]*src[1][2]) * idet;
    t[0][1] = -(src[0][1]*src[2][2] - src[2][1]*src[0][2]) * idet;
    t[0][2] =  (src[0][1]*src[1][2] - src[1][1]*src[0][2]) * idet;
    t[1][0] = -(src[1][0]*src[2][2] - src[2][0]*src[1][2]) * idet;
    t[1][1] =  (src[0][0]*src[2][2] - src[2][0]*src[0][2]) * idet;
    t[1][2] = -(src[0][0]*src[1][2] - src[1][0]*src[0][2]) * idet;
    t[2][0] =  (src[1][0]*src[2][1] - src[2][0]*src[1][1]) * idet;
    t[2][1] = -(src[0][0]*src[2][1] - src[2][0]*src[0][1]) * idet;
    t[2][2] =  (src[0][0]*src[1][1] - src[1][0]*src[0][1]) * idet;
    // Inverse translation = -inv3x3 * orig_translation.
    t[0][3] = -(t[0][0]*src[0][3] + t[0][1]*src[1][3] + t[0][2]*src[2][3]);
    t[1][3] = -(t[1][0]*src[0][3] + t[1][1]*src[1][3] + t[1][2]*src[2][3]);
    t[2][3] = -(t[2][0]*src[0][3] + t[2][1]*src[1][3] + t[2][2]*src[2][3]);
    PSMTXCopy(t, inv);
    return 1;
}

// Inverse-transpose of the 3x3 (normal matrix); translation column zeroed.
u32 C_MTXInvXpose(Mtx src, Mtx invX) {
    Mtx tmp;
    if (PSMTXInverse(src, tmp) == 0) return 0;
    invX[0][0] = tmp[0][0]; invX[0][1] = tmp[1][0]; invX[0][2] = tmp[2][0]; invX[0][3] = 0.0f;
    invX[1][0] = tmp[0][1]; invX[1][1] = tmp[1][1]; invX[1][2] = tmp[2][1]; invX[1][3] = 0.0f;
    invX[2][0] = tmp[0][2]; invX[2][1] = tmp[1][2]; invX[2][2] = tmp[2][2]; invX[2][3] = 0.0f;
    return 1;
}

void PSMTXRotTrig(Mtx m, char axis, f32 sinA, f32 cosA) {
    axis |= 0x20;  // tolower (matches the asm `ori axis,axis,0x20`)
    switch (axis) {
    case 'x':
        m[0][0] = 1.0f; m[0][1] = 0.0f;  m[0][2] = 0.0f;   m[0][3] = 0.0f;
        m[1][0] = 0.0f; m[1][1] = cosA;  m[1][2] = -sinA;  m[1][3] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = sinA;  m[2][2] = cosA;   m[2][3] = 0.0f;
        break;
    case 'y':
        m[0][0] = cosA;  m[0][1] = 0.0f; m[0][2] = sinA;  m[0][3] = 0.0f;
        m[1][0] = 0.0f;  m[1][1] = 1.0f; m[1][2] = 0.0f;  m[1][3] = 0.0f;
        m[2][0] = -sinA; m[2][1] = 0.0f; m[2][2] = cosA;  m[2][3] = 0.0f;
        break;
    case 'z':
        m[0][0] = cosA; m[0][1] = -sinA; m[0][2] = 0.0f; m[0][3] = 0.0f;
        m[1][0] = sinA; m[1][1] = cosA;  m[1][2] = 0.0f; m[1][3] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = 0.0f;  m[2][2] = 1.0f; m[2][3] = 0.0f;
        break;
    default:
        break;
    }
}

void PSMTXRotRad(Mtx m, char axis, f32 rad) {
    PSMTXRotTrig(m, axis, sinf(rad), cosf(rad));
}

// Rodrigues rotation about an arbitrary axis (decomp normalizes the axis first).
void PSMTXRotAxisRad(Mtx m, Vec* axis, f32 rad) {
    f32 s = sinf(rad), c = cosf(rad), t = 1.0f - c;
    f32 len = sqrtf(axis->x*axis->x + axis->y*axis->y + axis->z*axis->z);
    f32 x = axis->x, y = axis->y, z = axis->z;
    if (len != 0.0f) { f32 inv = 1.0f / len; x *= inv; y *= inv; z *= inv; }
    m[0][0] = t*x*x + c;   m[0][1] = t*x*y - s*z; m[0][2] = t*x*z + s*y; m[0][3] = 0.0f;
    m[1][0] = t*x*y + s*z; m[1][1] = t*y*y + c;   m[1][2] = t*y*z - s*x; m[1][3] = 0.0f;
    m[2][0] = t*x*z - s*y; m[2][1] = t*y*z + s*x; m[2][2] = t*z*z + c;   m[2][3] = 0.0f;
}

void PSMTXTrans(Mtx m, f32 xT, f32 yT, f32 zT) {
    m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = xT;
    m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f; m[1][3] = yT;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f; m[2][3] = zT;
}

void PSMTXTransApply(Mtx src, Mtx dst, f32 xT, f32 yT, f32 zT) {
    if (src != dst) {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) dst[i][j] = src[i][j];
    }
    dst[0][3] = src[0][3] + xT;
    dst[1][3] = src[1][3] + yT;
    dst[2][3] = src[2][3] + zT;
}

void PSMTXScale(Mtx m, f32 xS, f32 yS, f32 zS) {
    m[0][0] = xS;   m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = 0.0f;
    m[1][0] = 0.0f; m[1][1] = yS;   m[1][2] = 0.0f; m[1][3] = 0.0f;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = zS;   m[2][3] = 0.0f;
}

void PSMTXScaleApply(Mtx src, Mtx dst, f32 xS, f32 yS, f32 zS) {
    for (int j = 0; j < 4; ++j) dst[0][j] = src[0][j] * xS;
    for (int j = 0; j < 4; ++j) dst[1][j] = src[1][j] * yS;
    for (int j = 0; j < 4; ++j) dst[2][j] = src[2][j] * zS;
}

// Quaternion {x,y,z,w} -> rotation matrix. scale = 2/|q|^2 (handles unnormalized q).
void PSMTXQuat(Mtx m, Quaternion* q) {
    f32 norm = q->x*q->x + q->y*q->y + q->z*q->z + q->w*q->w;
    f32 s = (norm > 0.0f) ? 2.0f / norm : 0.0f;
    f32 xs = q->x*s,  ys = q->y*s,  zs = q->z*s;
    f32 wx = q->w*xs, wy = q->w*ys, wz = q->w*zs;
    f32 xx = q->x*xs, xy = q->x*ys, xz = q->x*zs;
    f32 yy = q->y*ys, yz = q->y*zs, zz = q->z*zs;
    m[0][0] = 1.0f - (yy + zz); m[0][1] = xy - wz;          m[0][2] = xz + wy;          m[0][3] = 0.0f;
    m[1][0] = xy + wz;          m[1][1] = 1.0f - (xx + zz); m[1][2] = yz - wx;          m[1][3] = 0.0f;
    m[2][0] = xz - wy;          m[2][1] = yz + wx;          m[2][2] = 1.0f - (xx + yy); m[2][3] = 0.0f;
}

// ===========================================================================
// MTX * VEC
// ===========================================================================

void PSMTXMultVec(Mtx m, Vec* src, Vec* dst) {
    f32 x = src->x, y = src->y, z = src->z;  // buffer for dst==src aliasing
    dst->x = m[0][0]*x + m[0][1]*y + m[0][2]*z + m[0][3];
    dst->y = m[1][0]*x + m[1][1]*y + m[1][2]*z + m[1][3];
    dst->z = m[2][0]*x + m[2][1]*y + m[2][2]*z + m[2][3];
}

// Scale+Rotate only (no translation).
void PSMTXMultVecSR(Mtx m, Vec* src, Vec* dst) {
    f32 x = src->x, y = src->y, z = src->z;
    dst->x = m[0][0]*x + m[0][1]*y + m[0][2]*z;
    dst->y = m[1][0]*x + m[1][1]*y + m[1][2]*z;
    dst->z = m[2][0]*x + m[2][1]*y + m[2][2]*z;
}

void PSMTXMultVecArray(Mtx m, Vec* srcBase, Vec* dstBase, u32 count) {
    for (u32 i = 0; i < count; ++i)
        PSMTXMultVec(m, &srcBase[i], &dstBase[i]);
}

// ===========================================================================
// VEC
// ===========================================================================

void PSVECAdd(Vec* a, Vec* b, Vec* c) {
    c->x = a->x + b->x; c->y = a->y + b->y; c->z = a->z + b->z;
}
void PSVECSubtract(Vec* a, Vec* b, Vec* c) {
    c->x = a->x - b->x; c->y = a->y - b->y; c->z = a->z - b->z;
}
void PSVECScale(Vec* src, Vec* dst, f32 mult) {
    dst->x = src->x * mult; dst->y = src->y * mult; dst->z = src->z * mult;
}
void PSVECNormalize(Vec* v, Vec* dst) {
    f32 mag2 = v->x*v->x + v->y*v->y + v->z*v->z;
    f32 rmag = 1.0f / sqrtf(mag2);
    dst->x = v->x * rmag; dst->y = v->y * rmag; dst->z = v->z * rmag;
}
f32 PSVECMag(Vec* v) {
    return sqrtf(v->x*v->x + v->y*v->y + v->z*v->z);
}
f32 PSVECDotProduct(Vec* a, Vec* b) {
    return a->x*b->x + a->y*b->y + a->z*b->z;
}
void PSVECCrossProduct(Vec* a, Vec* b, Vec* dst) {
    f32 x = a->y*b->z - a->z*b->y;   // buffer for aliasing
    f32 y = a->z*b->x - a->x*b->z;
    f32 z = a->x*b->y - a->y*b->x;
    dst->x = x; dst->y = y; dst->z = z;
}
f32 PSVECSquareDistance(Vec* a, Vec* b) {
    f32 dx = a->x-b->x, dy = a->y-b->y, dz = a->z-b->z;
    return dx*dx + dy*dy + dz*dz;
}
f32 PSVECDistance(Vec* a, Vec* b) {
    return sqrtf(PSVECSquareDistance(a, b));
}

// ===========================================================================
// C_MTX projection / lookat — ported verbatim from the decomp (mtx.c / mtx44.c).
// ===========================================================================

void C_MTXLookAt(Mtx m, Point3dPtr camPos, VecPtr camUp, Point3dPtr target) {
    Vec vLook, vRight, vUp;
    vLook.x = camPos->x - target->x;
    vLook.y = camPos->y - target->y;
    vLook.z = camPos->z - target->z;
    PSVECNormalize(&vLook, &vLook);
    PSVECCrossProduct(camUp, &vLook, &vRight);
    PSVECNormalize(&vRight, &vRight);
    PSVECCrossProduct(&vLook, &vRight, &vUp);

    m[0][0] = vRight.x; m[0][1] = vRight.y; m[0][2] = vRight.z;
    m[0][3] = -((camPos->z*vRight.z) + ((camPos->x*vRight.x) + (camPos->y*vRight.y)));
    m[1][0] = vUp.x; m[1][1] = vUp.y; m[1][2] = vUp.z;
    m[1][3] = -((camPos->z*vUp.z) + ((camPos->x*vUp.x) + (camPos->y*vUp.y)));
    m[2][0] = vLook.x; m[2][1] = vLook.y; m[2][2] = vLook.z;
    m[2][3] = -((camPos->z*vLook.z) + ((camPos->x*vLook.x) + (camPos->y*vLook.y)));
}

void C_MTXFrustum(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f) {
    f32 tmp;
    tmp = 1.0f / (r - l);
    m[0][0] = (2*n)*tmp; m[0][1] = 0.0f; m[0][2] = (r+l)*tmp; m[0][3] = 0.0f;
    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f; m[1][1] = (2*n)*tmp; m[1][2] = (t+b)*tmp; m[1][3] = 0.0f;
    m[2][0] = 0.0f; m[2][1] = 0.0f;
    tmp = 1.0f / (f - n);
    m[2][2] = -(n)*tmp; m[2][3] = -(f*n)*tmp;
    m[3][0] = 0.0f; m[3][1] = 0.0f; m[3][2] = -1.0f; m[3][3] = 0.0f;
}

void C_MTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 n, f32 f) {
    f32 angle = (0.5f * fovY) * 0.017453293f;
    f32 cot = 1.0f / tanf(angle);
    f32 tmp;
    m[0][0] = cot/aspect; m[0][1] = 0; m[0][2] = 0; m[0][3] = 0;
    m[1][0] = 0; m[1][1] = cot; m[1][2] = 0; m[1][3] = 0;
    m[2][0] = 0; m[2][1] = 0;
    tmp = 1.0f / (f - n);
    m[2][2] = -n*tmp; m[2][3] = tmp*-(f*n);
    m[3][0] = 0; m[3][1] = 0; m[3][2] = -1; m[3][3] = 0;
}

void C_MTXOrtho(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f) {
    f32 tmp;
    tmp = 1.0f / (r - l);
    m[0][0] = 2*tmp; m[0][1] = 0; m[0][2] = 0; m[0][3] = tmp*-(r+l);
    tmp = 1.0f / (t - b);
    m[1][0] = 0; m[1][1] = 2*tmp; m[1][2] = 0; m[1][3] = tmp*-(t+b);
    m[2][0] = 0; m[2][1] = 0;
    tmp = 1.0f / (f - n);
    m[2][2] = -1*tmp; m[2][3] = -f*tmp;
    m[3][0] = 0; m[3][1] = 0; m[3][2] = 0; m[3][3] = 1;
}

void C_MTXLightFrustum(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 scaleS,
                       f32 scaleT, f32 transS, f32 transT) {
    f32 _tmp;
    _tmp = 1.0f / (r - l);
    m[0][0] = scaleS * (2*n*_tmp); m[0][1] = 0;
    m[0][2] = (scaleS * (_tmp*(r+l))) - transS; m[0][3] = 0;
    _tmp = 1.0f / (t - b);
    m[1][0] = 0; m[1][1] = scaleT * (2*n*_tmp);
    m[1][2] = (scaleT * (_tmp*(t+b))) - transT; m[1][3] = 0;
    m[2][0] = 0; m[2][1] = 0; m[2][2] = -1; m[2][3] = 0;
}

void C_MTXLightPerspective(Mtx m, f32 fovY, f32 aspect, f32 scaleS, f32 scaleT,
                           f32 transS, f32 transT) {
    f32 angle = (0.5f * fovY) * 0.017453293f;
    f32 cot = 1.0f / tanf(angle);
    m[0][0] = scaleS * (cot/aspect); m[0][1] = 0; m[0][2] = -transS; m[0][3] = 0;
    m[1][0] = 0; m[1][1] = cot*scaleT; m[1][2] = -transT; m[1][3] = 0;
    m[2][0] = 0; m[2][1] = 0; m[2][2] = -1; m[2][3] = 0;
}

void C_MTXLightOrtho(Mtx m, f32 t, f32 b, f32 l, f32 r, f32 scaleS, f32 scaleT,
                     f32 transS, f32 transT) {
    f32 _tmp;
    _tmp = 1.0f / (r - l);
    m[0][0] = 2*_tmp*scaleS; m[0][1] = 0; m[0][2] = 0;
    m[0][3] = transS + (scaleS * (_tmp*-(r+l)));
    _tmp = 1.0f / (t - b);
    m[1][0] = 0; m[1][1] = 2*_tmp*scaleT; m[1][2] = 0;
    m[1][3] = transT + (scaleT * (_tmp*-(t+b)));
    m[2][0] = 0; m[2][1] = 0; m[2][2] = 0; m[2][3] = 1;
}

} // extern "C"
