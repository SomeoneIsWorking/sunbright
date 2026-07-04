// mtx_test.cpp — TDD harness for the MTX/VEC seam (native/platform/mtx_impl.cpp).
//
// Asserts SPEC-COMPUTED ground truth (hand-derived), not eyeballed output. Calls the
// SHIPPING extern "C" functions directly (no forked copy). Build/run via the
// `sms-platform-test` CMake target (ctest -R platform_test) or run the binary directly;
// returns nonzero on any failure.

#include <dolphin/mtx.h>
#include <cmath>
#include <cstdio>

static int g_fail = 0;
static int g_checks = 0;

static void chk(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("  FAIL: %s\n", what); }
}
static bool feq(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }
static void chkf(f32 got, f32 want, const char* what, f32 eps = 1e-4f) {
    ++g_checks;
    if (!feq(got, want, eps)) {
        ++g_fail;
        std::printf("  FAIL: %s  got=%.6f want=%.6f\n", what, got, want);
    }
}
static void chkmtx(Mtx got, const f32 want[3][4], const char* what) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) {
            ++g_checks;
            if (!feq(got[i][j], want[i][j])) {
                ++g_fail;
                std::printf("  FAIL: %s [%d][%d] got=%.6f want=%.6f\n",
                            what, i, j, got[i][j], want[i][j]);
            }
        }
}

static void test_basic() {
    Mtx I;
    PSMTXIdentity(I);
    const f32 wantI[3][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0}};
    chkmtx(I, wantI, "Identity");

    Mtx cp;
    PSMTXCopy(I, cp);
    chkmtx(cp, wantI, "Copy");

    Mtx tr;
    PSMTXTrans(tr, 1, 2, 3);
    const f32 wantTr[3][4] = {{1,0,0,1},{0,1,0,2},{0,0,1,3}};
    chkmtx(tr, wantTr, "Trans");

    Mtx sc;
    PSMTXScale(sc, 2, 3, 4);
    const f32 wantSc[3][4] = {{2,0,0,0},{0,3,0,0},{0,0,4,0}};
    chkmtx(sc, wantSc, "Scale");
}

static void test_concat() {
    // ab = a*b ; a=T(10,20,30), b=T(1,2,3) ; applied to origin -> (11,22,33).
    Mtx a, b, ab;
    PSMTXTrans(a, 10, 20, 30);
    PSMTXTrans(b, 1, 2, 3);
    PSMTXConcat(a, b, ab);
    Vec o = {0, 0, 0}, r;
    PSMTXMultVec(ab, &o, &r);
    chkf(r.x, 11, "Concat.x"); chkf(r.y, 22, "Concat.y"); chkf(r.z, 33, "Concat.z");

    // identity*M == M ; and aliasing ab==a.
    Mtx I;
    PSMTXIdentity(I);
    Mtx m;
    PSMTXTrans(m, 5, 6, 7);
    PSMTXConcat(I, m, m);  // aliased output
    const f32 wantM[3][4] = {{1,0,0,5},{0,1,0,6},{0,0,1,7}};
    chkmtx(m, wantM, "Concat-identity-aliased");
}

static void test_multvec() {
    Mtx m;
    PSMTXTrans(m, 1, 2, 3);
    Vec v = {4, 5, 6}, r;
    PSMTXMultVec(m, &v, &r);
    chkf(r.x, 5, "MultVec.x"); chkf(r.y, 7, "MultVec.y"); chkf(r.z, 9, "MultVec.z");
    // SR ignores translation.
    PSMTXMultVecSR(m, &v, &r);
    chkf(r.x, 4, "MultVecSR.x"); chkf(r.y, 5, "MultVecSR.y"); chkf(r.z, 6, "MultVecSR.z");
    // dst==src aliasing safe.
    Vec a = {4, 5, 6};
    PSMTXMultVec(m, &a, &a);
    chkf(a.x, 5, "MultVec-aliased.x");
}

static void test_rot() {
    // RotTrig Z by 90deg: (1,0,0) -> (0,1,0).
    Mtx z;
    PSMTXRotTrig(z, 'z', 1.0f /*sin90*/, 0.0f /*cos90*/);
    Vec x = {1, 0, 0}, r;
    PSMTXMultVecSR(z, &x, &r);
    chkf(r.x, 0, "RotZ.x"); chkf(r.y, 1, "RotZ.y"); chkf(r.z, 0, "RotZ.z");

    // RotRad about Z (pi/2) matches.
    Mtx zr;
    PSMTXRotRad(zr, 'Z', (f32)M_PI / 2.0f);  // uppercase axis -> tolower'd
    PSMTXMultVecSR(zr, &x, &r);
    chkf(r.x, 0, "RotRadZ.x"); chkf(r.y, 1, "RotRadZ.y");

    // RotAxisRad about (0,0,1) by pi/2 == RotTrig Z.
    Mtx za;
    Vec axis = {0, 0, 1};
    PSMTXRotAxisRad(za, &axis, (f32)M_PI / 2.0f);
    PSMTXMultVecSR(za, &x, &r);
    chkf(r.x, 0, "RotAxisZ.x"); chkf(r.y, 1, "RotAxisZ.y");

    // RotTrig X by 90: (0,1,0) -> (0,0,1).
    Mtx xm;
    PSMTXRotTrig(xm, 'x', 1.0f, 0.0f);
    Vec yv = {0, 1, 0};
    PSMTXMultVecSR(xm, &yv, &r);
    chkf(r.x, 0, "RotX.x"); chkf(r.y, 0, "RotX.y"); chkf(r.z, 1, "RotX.z");
}

static void test_quat() {
    // Identity quaternion {0,0,0,1} -> identity matrix.
    Mtx q;
    Quaternion qi = {0, 0, 0, 1};
    PSMTXQuat(q, &qi);
    const f32 wantI[3][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0}};
    chkmtx(q, wantI, "QuatIdentity");

    // 90deg about Z: q = (0,0,sin45,cos45) -> matches RotTrig Z 90.
    f32 s = sinf((f32)M_PI / 4.0f), c = cosf((f32)M_PI / 4.0f);
    Quaternion qz = {0, 0, s, c};
    PSMTXQuat(q, &qz);
    Vec x = {1, 0, 0}, r;
    PSMTXMultVecSR(q, &x, &r);
    chkf(r.x, 0, "QuatZ.x"); chkf(r.y, 1, "QuatZ.y"); chkf(r.z, 0, "QuatZ.z");
}

static void test_inverse() {
    // M = RotZ(37deg) with translation; M * inv(M) == I.
    Mtx m;
    f32 a = 0.6457718f;  // ~37deg
    PSMTXRotTrig(m, 'z', sinf(a), cosf(a));
    m[0][3] = 12.0f; m[1][3] = -3.5f; m[2][3] = 7.0f;

    Mtx inv;
    u32 ok = PSMTXInverse(m, inv);
    chk(ok == 1, "Inverse-nonsingular");

    Mtx prod;
    PSMTXConcat(m, inv, prod);
    const f32 wantI[3][4] = {{1,0,0,0},{0,1,0,0},{0,0,1,0}};
    chkmtx(prod, wantI, "M*inv(M)==I");

    // Round-trip a point.
    Vec p = {3, 4, 5}, tp, back;
    PSMTXMultVec(m, &p, &tp);
    PSMTXMultVec(inv, &tp, &back);
    chkf(back.x, 3, "inv-roundtrip.x"); chkf(back.y, 4, "inv-roundtrip.y");
    chkf(back.z, 5, "inv-roundtrip.z");

    // Singular -> returns 0.
    Mtx zero = {{0,0,0,0},{0,0,0,0},{0,0,0,0}}, dummy;
    chk(PSMTXInverse(zero, dummy) == 0, "Inverse-singular-returns-0");
}

static void test_transpose() {
    Mtx m;
    PSMTXRotTrig(m, 'z', 1.0f, 0.0f);  // RotZ 90
    m[0][3] = 9; m[1][3] = 9; m[2][3] = 9;  // translation must be zeroed by transpose
    Mtx t;
    PSMTXTranspose(m, t);
    // Transpose of RotZ90 = RotZ(-90).
    const f32 want[3][4] = {{0,1,0,0},{-1,0,0,0},{0,0,1,0}};
    chkmtx(t, want, "Transpose");
}

static void test_vec() {
    Vec a = {1, 2, 3}, b = {4, 5, 6}, r;
    PSVECAdd(&a, &b, &r);
    chkf(r.x, 5, "VECAdd.x"); chkf(r.y, 7, "VECAdd.y"); chkf(r.z, 9, "VECAdd.z");
    PSVECSubtract(&b, &a, &r);
    chkf(r.x, 3, "VECSub.x"); chkf(r.y, 3, "VECSub.y"); chkf(r.z, 3, "VECSub.z");
    PSVECScale(&a, &r, 2.0f);
    chkf(r.x, 2, "VECScale.x"); chkf(r.z, 6, "VECScale.z");
    chkf(PSVECDotProduct(&a, &b), 32, "VECDot");  // 4+10+18

    // cross of x,y unit -> z unit.
    Vec ux = {1, 0, 0}, uy = {0, 1, 0};
    PSVECCrossProduct(&ux, &uy, &r);
    chkf(r.x, 0, "VECCross.x"); chkf(r.y, 0, "VECCross.y"); chkf(r.z, 1, "VECCross.z");
    // cross aliasing: dst==a.
    Vec ax = {1, 0, 0};
    PSVECCrossProduct(&ax, &uy, &ax);
    chkf(ax.z, 1, "VECCross-aliased.z");

    Vec v = {3, 4, 0};
    chkf(PSVECMag(&v), 5, "VECMag");
    PSVECNormalize(&v, &r);
    chkf(r.x, 0.6f, "VECNorm.x"); chkf(r.y, 0.8f, "VECNorm.y");
    chkf(PSVECMag(&r), 1.0f, "VECNorm-unit");

    Vec p0 = {0, 0, 0}, p1 = {3, 4, 0};
    chkf(PSVECSquareDistance(&p0, &p1), 25, "VECSqDist");
    chkf(PSVECDistance(&p0, &p1), 5, "VECDist");
}

static void test_proj() {
    // C_MTXOrtho: maps l,r,t,b,n,f. Check known entries.
    Mtx44 o;
    C_MTXOrtho(o, /*t*/1, /*b*/-1, /*l*/-1, /*r*/1, /*n*/0, /*f*/-10);
    chkf(o[0][0], 1.0f, "Ortho00");       // 2/(r-l)=2/2=1
    chkf(o[1][1], 1.0f, "Ortho11");       // 2/(t-b)=2/2=1
    chkf(o[3][3], 1.0f, "Ortho33");
    chkf(o[2][2], -1.0f / (-10 - 0), "Ortho22");

    // C_MTXPerspective: m11 = cot(fov/2); aspect scales m00.
    Mtx44 p;
    C_MTXPerspective(p, /*fovY*/90.0f, /*aspect*/1.0f, /*n*/1.0f, /*f*/100.0f);
    f32 cot = 1.0f / tanf(45.0f * 0.017453293f);  // ~1
    chkf(p[1][1], cot, "Persp11", 1e-3f);
    chkf(p[0][0], cot, "Persp00", 1e-3f);
    chkf(p[3][2], -1.0f, "Persp32");

    // C_MTXLookAt: camera at +Z looking at origin, up +Y -> identity-ish basis.
    Mtx la;
    Vec camPos = {0, 0, 10}, up = {0, 1, 0}, target = {0, 0, 0};
    C_MTXLookAt(la, &camPos, &up, &target);
    // vLook = normalize(camPos-target) = +Z. right = up x look = (0,1,0)x(0,0,1)=(1,0,0).
    chkf(la[0][0], 1, "LookAt right.x");
    chkf(la[1][1], 1, "LookAt up.y");
    chkf(la[2][2], 1, "LookAt look.z");
    chkf(la[2][3], -10, "LookAt look.trans");  // -(camPos . look) = -(10*1)
}

int main() {
    std::printf("== MTX/VEC seam unit tests ==\n");
    test_basic();
    test_concat();
    test_multvec();
    test_rot();
    test_quat();
    test_inverse();
    test_transpose();
    test_vec();
    test_proj();
    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
