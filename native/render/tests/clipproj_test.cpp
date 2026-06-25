// clipproj_test.cpp — prove the PC-way clip-space projection (imm_project_eye_clip) divides to
// EXACTLY the NDC the old CPU-divide path (imm_project_eye) produced. This is the falsifiable gate
// for the renderer rewrite that dropped the w=1 affine path + the hand-rolled NDC clipper: the
// on-screen position and depth must be bit-for-bit identical (only the interpolation/clipping moved
// to the GPU). Pure unit — no Vulkan, no GPU, no ROM.

#include "gx_imm_xform.h"
#include <cstdio>
#include <cmath>

using namespace sb::render;

static int g_fail = 0, g_checks = 0;
static bool feq(float a, float b, float e = 1e-4f) { float d = a - b; return (d < 0 ? -d : d) <= e; }
static void chk(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("  FAIL: %s\n", what); }
}

// Assert imm_project_eye_clip(eye).{x/w,y/w,z/w} == imm_project_eye(eye).{x,y,z}.
static void cmp(float ex, float ey, float ez, int pt, const float proj[6], const float vp[6],
                const char* tag) {
    SbImmVtx  n = imm_project_eye(ex, ey, ez, pt, proj, vp);
    SbImmClip c = imm_project_eye_clip(ex, ey, ez, pt, proj, vp);
    chk(c.w != 0.0f, tag);
    const float nx = c.x / c.w, ny = c.y / c.w, nz = c.z / c.w;
    bool ok = feq(nx, n.x) && feq(ny, n.y) && feq(nz, n.z);
    if (!ok) std::printf("  [%s] clip/w=(%.4f,%.4f,%.4f) expected NDC=(%.4f,%.4f,%.4f) w=%.3f\n",
                         tag, nx, ny, nz, n.x, n.y, n.z, c.w);
    chk(ok, tag);
}

int main() {
    std::printf("== clip-space projection divides to the old NDC ==\n");

    // A typical SMS perspective frustum (C_MTXPerspective-like) decomposed to the 6 gx->projMtx
    // slots: pm[0]=cot/aspect, pm[1]=0(centred), pm[2]=cot, pm[3]=0, pm[4]=f/(n-f), pm[5]=nf/(n-f).
    const float fov = 60.0f * 3.14159265f / 180.0f, aspect = 16.0f/9.0f, n = 1.0f, f = 5000.0f;
    const float cot = 1.0f / std::tan(fov * 0.5f);
    const float persp[6] = { cot/aspect, 0.0f, cot, 0.0f, f/(n-f), (f*n)/(n-f) };

    // Viewport WITH a nonzero offset (the derivation's 2*vp0/vp2 / 2*vp1/vp3 terms must hold).
    const float vpOff[6] = { 32.0f, 16.0f, 1280.0f, 720.0f, 0.0f, 1.0f };
    const float vp00[6]  = {  0.0f,  0.0f,  640.0f, 480.0f, 0.0f, 1.0f };

    // Perspective: in-front-of-camera points (ez<0) across the frustum.
    const float pts[][3] = {
        {  0.0f,   0.0f,  -10.0f},   // straight ahead
        { 50.0f,  30.0f,  -10.0f},   // off to upper-right (near)
        {-400.0f,-200.0f,-2000.0f},  // far, lower-left
        {120.0f, -80.0f,  -3.0f},    // close to the near plane
    };
    for (int i = 0; i < 4; ++i) {
        char tag[48]; std::snprintf(tag, sizeof tag, "persp vpOff pt%d", i);
        cmp(pts[i][0], pts[i][1], pts[i][2], 0, persp, vpOff, tag);
        std::snprintf(tag, sizeof tag, "persp vp00 pt%d", i);
        cmp(pts[i][0], pts[i][1], pts[i][2], 0, persp, vp00, tag);
    }

    // Orthographic: pm[0]=2/(r-l), pm[1]=-(r+l)/(r-l), pm[2]=2/(t-b), pm[3]=-(t+b)/(t-b),
    // pm[4]=-1/(f-n) (or similar), pm[5]=-n/(f-n). w must come back 1.0.
    const float ortho[6] = { 2.0f/640.0f, 0.0f, 2.0f/480.0f, 0.0f, -1.0f/(f-n), -n/(f-n) };
    const float opts[][3] = { {0,0,-1}, {100,-50,-200}, {-300,200,-1000} };
    for (int i = 0; i < 3; ++i) {
        char tag[48]; std::snprintf(tag, sizeof tag, "ortho pt%d", i);
        SbImmClip c = imm_project_eye_clip(opts[i][0], opts[i][1], opts[i][2], 1, ortho, vpOff);
        chk(feq(c.w, 1.0f), tag);
        cmp(opts[i][0], opts[i][1], opts[i][2], 1, ortho, vpOff, tag);
    }

    // Perspective w is +ve in front of the camera and -ve behind it (so the GPU near-clips behind-
    // camera verts instead of the CPU exploding them through the 1/-ez divide).
    chk(imm_project_eye_clip(0,0,-10, 0, persp, vp00).w > 0.0f, "w>0 in front of camera");
    chk(imm_project_eye_clip(0,0, 10, 0, persp, vp00).w < 0.0f, "w<0 behind camera");

    std::printf("%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
