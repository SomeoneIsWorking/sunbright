// ===========================================================================
// port/pal/mtx_test.cpp — unit tests for the portable GC MTX/VEC PAL.
//
// Checks mathematical identities (not bit-for-bit, since these run on host
// float HW): Concat with identity, Concat+MultVec vs manual transform,
// Inverse*M ~= identity, Normalize -> unit length, Transpose, projection
// matrix entries for known inputs, and the singular-matrix return code.
//
// Build links port/pal/mtx.cpp. Exits non-zero if any check fails.
// ===========================================================================

#include <dolphin/mtx.h>
#include <cmath>
#include <cstdio>

// MarioUtil Ms* helpers (C++ linkage).
// (MsVECNormalize/MsVECMag2 are MarioUtil — they now live in the core lib's
//  port/src/MathUtil.cpp, not the math PAL — covered by that file's own test.)

static int g_pass = 0;
static int g_fail = 0;

static void check(const char* name, bool ok)
{
    if (ok) { ++g_pass; std::printf("  PASS  %s\n", name); }
    else    { ++g_fail; std::printf("  FAIL  %s\n", name); }
}

static bool fclose_(float a, float b, float eps = 1e-4f)
{
    return std::fabs(a - b) <= eps * (1.0f + std::fabs(a) + std::fabs(b));
}

static bool mtx_close(Mtx a, Mtx b, float eps = 1e-3f)
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            if (!fclose_(a[i][j], b[i][j], eps)) return false;
    return true;
}

int main()
{
    std::printf("== PAL mtx/vec unit tests ==\n");

    // --- Concat with identity ---
    {
        Mtx M = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}};
        Mtx I, out;
        PSMTXIdentity(I);
        PSMTXConcat(M, I, out);
        check("Concat(M, identity) == M", mtx_close(out, M));
        PSMTXConcat(I, M, out);
        check("Concat(identity, M) == M", mtx_close(out, M));
    }

    // --- Concat then MultVec equals manual composed transform ---
    {
        Mtx T, R, TR;
        PSMTXTrans(T, 10.0f, 20.0f, 30.0f);
        PSMTXRotRad(R, 'z', (float)(M_PI / 2.0));  // 90 deg about z
        PSMTXConcat(T, R, TR);                     // first rotate, then translate

        Vec v = {1.0f, 0.0f, 0.0f};
        Vec viaConcat, viaSteps, tmp;
        PSMTXMultVec(TR, &v, &viaConcat);
        PSMTXMultVec(R, &v, &tmp);
        PSMTXMultVec(T, &tmp, &viaSteps);
        check("Concat(T,R)*v == T*(R*v)",
              fclose_(viaConcat.x, viaSteps.x) &&
              fclose_(viaConcat.y, viaSteps.y) &&
              fclose_(viaConcat.z, viaSteps.z));
        // 90deg z-rot of (1,0,0) -> (0,1,0); then +translate -> (10,21,30)
        check("Rz(90)*(1,0,0)+T == (10,21,30)",
              fclose_(viaConcat.x, 10.0f) &&
              fclose_(viaConcat.y, 21.0f) &&
              fclose_(viaConcat.z, 30.0f));
    }

    // --- MultVecSR ignores translation ---
    {
        Mtx T;
        PSMTXTrans(T, 5.0f, 6.0f, 7.0f);
        Vec v = {1.0f, 2.0f, 3.0f}, out;
        PSMTXMultVecSR(T, &v, &out);   // identity rotation, no translation
        check("MultVecSR ignores translation",
              fclose_(out.x, 1.0f) && fclose_(out.y, 2.0f) && fclose_(out.z, 3.0f));
    }

    // --- Inverse * M ~= identity (affine with rotation+translation+scale) ---
    {
        Mtx R, T, S, RT, M, Inv, prod;
        PSMTXRotRad(R, 'y', 0.7f);
        PSMTXScale(S, 2.0f, 3.0f, 0.5f);
        PSMTXConcat(R, S, RT);
        PSMTXTrans(T, 11.0f, -4.0f, 9.0f);
        PSMTXConcat(T, RT, M);          // M = T * R * S
        u32 ok = PSMTXInverse(M, Inv);
        check("Inverse returns 1 for non-singular", ok == 1);
        PSMTXConcat(M, Inv, prod);
        Mtx I; PSMTXIdentity(I);
        check("M * Inverse(M) ~= identity", mtx_close(prod, I, 2e-3f));
        PSMTXConcat(Inv, M, prod);
        check("Inverse(M) * M ~= identity", mtx_close(prod, I, 2e-3f));
    }

    // --- Inverse of singular matrix returns 0 ---
    {
        Mtx Z = {{0,0,0,0},{0,0,0,0},{0,0,0,0}}, Inv;
        u32 ok = PSMTXInverse(Z, Inv);
        check("Inverse(singular) returns 0", ok == 0);
    }

    // --- Transpose ---
    {
        Mtx M = {{1, 2, 3, 99}, {4, 5, 6, 99}, {7, 8, 9, 99}}, X;
        PSMTXTranspose(M, X);
        bool ok = fclose_(X[0][0],1)&&fclose_(X[0][1],4)&&fclose_(X[0][2],7)&&
                  fclose_(X[1][0],2)&&fclose_(X[1][1],5)&&fclose_(X[1][2],8)&&
                  fclose_(X[2][0],3)&&fclose_(X[2][1],6)&&fclose_(X[2][2],9)&&
                  fclose_(X[0][3],0)&&fclose_(X[1][3],0)&&fclose_(X[2][3],0);
        check("Transpose 3x3 + zero translation col", ok);
    }

    // --- Vector ops ---
    {
        Vec a = {3.0f, 0.0f, 4.0f}, u;
        PSVECNormalize(&a, &u);
        float mag = std::sqrt(u.x*u.x + u.y*u.y + u.z*u.z);
        check("Normalize -> unit length", fclose_(mag, 1.0f, 1e-3f));

        float m = PSVECMag(&a);
        check("Mag(3,0,4) == 5", fclose_(m, 5.0f, 1e-3f));

        Vec z = {0,0,0}, zu;
        PSVECNormalize(&z, &zu);   // must not produce NaN-propagating crash
        check("Mag(0,0,0) == 0", fclose_(PSVECMag(&z), 0.0f, 1e-6f));

        Vec p = {1, 2, 3}, q = {4, 5, 6}, c;
        check("Dot((1,2,3),(4,5,6)) == 32", fclose_(PSVECDotProduct(&p,&q), 32.0f));
        PSVECCrossProduct(&p, &q, &c);
        // (2*6-3*5, 3*4-1*6, 1*5-2*4) = (-3, 6, -3)
        check("Cross((1,2,3),(4,5,6)) == (-3,6,-3)",
              fclose_(c.x,-3.0f)&&fclose_(c.y,6.0f)&&fclose_(c.z,-3.0f));
        Vec s;
        PSVECAdd(&p, &q, &s);
        check("Add == (5,7,9)", fclose_(s.x,5)&&fclose_(s.y,7)&&fclose_(s.z,9));
        PSVECSubtract(&q, &p, &s);
        check("Subtract == (3,3,3)", fclose_(s.x,3)&&fclose_(s.y,3)&&fclose_(s.z,3));
        PSVECScale(&p, &s, 2.0f);
        check("Scale x2 == (2,4,6)", fclose_(s.x,2)&&fclose_(s.y,4)&&fclose_(s.z,6));
        Vec d0 = {0,0,0}, d1 = {0,3,4};
        check("Distance((0,0,0),(0,3,4)) == 5", fclose_(PSVECDistance(&d0,&d1), 5.0f, 1e-3f));
        check("SquareMag(3,0,4) == 25", fclose_(PSVECSquareMag(&a), 25.0f));
    }

    // --- C_MTXOrtho known entries ---
    {
        Mtx44 m;
        // t=1 b=-1 l=-1 r=1 n=0 f=-100  (typical 2D ortho)
        C_MTXOrtho(m, 1.0f, -1.0f, -1.0f, 1.0f, 0.0f, -100.0f);
        // m[0][0]=2/(r-l)=1 ; m[1][1]=2/(t-b)=1 ; m[3][3]=1
        check("Ortho m00 == 1", fclose_(m[0][0], 1.0f));
        check("Ortho m11 == 1", fclose_(m[1][1], 1.0f));
        check("Ortho m33 == 1", fclose_(m[3][3], 1.0f));
        check("Ortho m03 == 0 (centered)", fclose_(m[0][3], 0.0f));
        // m[2][2] = -1/(f-n) = -1/(-100-0) = 0.01
        check("Ortho m22 == 0.01", fclose_(m[2][2], 0.01f));
    }

    // --- C_MTXPerspective known entries ---
    {
        Mtx44 m;
        // 90deg fovY, aspect 1 -> cot(45deg)=1 -> m00=m11=1
        C_MTXPerspective(m, 90.0f, 1.0f, 1.0f, 100.0f);
        check("Perspective m00 == 1 (fov90 aspect1)", fclose_(m[0][0], 1.0f, 1e-3f));
        check("Perspective m11 == 1 (fov90)", fclose_(m[1][1], 1.0f, 1e-3f));
        check("Perspective m32 == -1", fclose_(m[3][2], -1.0f));
        check("Perspective m33 == 0", fclose_(m[3][3], 0.0f));
        // aspect 2 -> m00 halves
        C_MTXPerspective(m, 90.0f, 2.0f, 1.0f, 100.0f);
        check("Perspective m00 halves with aspect 2", fclose_(m[0][0], 0.5f, 1e-3f));
    }

    // --- C_MTXLookAt: camera at +Z looking at origin, up +Y ---
    {
        Mtx m;
        Vec camPos = {0, 0, 10}, up = {0, 1, 0}, target = {0, 0, 0};
        C_MTXLookAt(m, &camPos, &up, &target);
        // look dir = camPos-target normalized = (0,0,1); right = up x look = (1,0,0);
        // up' = look x right = (0,1,0). Row0=right, Row1=up', Row2=look.
        check("LookAt right row == (1,0,0)",
              fclose_(m[0][0],1)&&fclose_(m[0][1],0)&&fclose_(m[0][2],0));
        check("LookAt up row == (0,1,0)",
              fclose_(m[1][0],0)&&fclose_(m[1][1],1)&&fclose_(m[1][2],0));
        check("LookAt look row == (0,0,1)",
              fclose_(m[2][0],0)&&fclose_(m[2][1],0)&&fclose_(m[2][2],1));
        // translation: m[2][3] = -(camPos.z*look.z) = -10
        check("LookAt translates z by -10", fclose_(m[2][3], -10.0f, 1e-3f));
    }

    // --- PSMTXQuat: identity quaternion (0,0,0,1) -> identity rotation ---
    {
        Quaternion q = {0, 0, 0, 1};
        Mtx m;
        PSMTXQuat(m, &q);
        Mtx I; PSMTXIdentity(I);
        check("Quat(identity) == identity rot", mtx_close(m, I));
        // 90deg about z: q = (0,0,sin45,cos45)
        float s = std::sin((float)(M_PI/4)), c = std::cos((float)(M_PI/4));
        Quaternion qz = {0, 0, s, c};
        PSMTXQuat(m, &qz);
        Vec v = {1,0,0}, out;
        PSMTXMultVecSR(m, &v, &out);
        check("Quat(Rz90)*(1,0,0) == (0,1,0)",
              fclose_(out.x,0,1e-3f)&&fclose_(out.y,1,1e-3f)&&fclose_(out.z,0,1e-3f));
    }

    std::printf("== %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
