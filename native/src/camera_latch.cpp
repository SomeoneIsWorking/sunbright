// camera_latch.cpp — own the gameplay camera transform for the native scene render.
//
// In sms-boot the GC draw phase doesn't run, so the active camera's perform() — which builds
// the view matrix (into j3dSys) and the perspective projection (GXSetProjection) — never
// executes. The model render therefore had an identity view + identity projection (geometry
// projected to garbage). This reads the active camera object (gpCamera) directly each frame
// and computes BOTH transforms ourselves, exactly as TLookAtCamera::perform does
// (C_MTXLookAt + C_MTXPerspective), then publishes them where the render reads them:
// j3dSys.mViewMtx (used by J3DModel::viewCalc) and the latched perspective 4x4.
// (Native-only TU — it is only ever added to the sms-native build, so no platform guard.)
#include <Camera/Camera.hpp>                          // gpCamera (CPolarSubCamera*)
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>        // j3dSys
#include <dolphin/mtx.h>
#include <cstdio>
#include <cstdlib>

extern "C" void sb_gx_latch_proj44(const float[16]);

extern "C" void sb_boot_latch_camera() {
    {
        static int dbg = -1;
        if (dbg < 0) { const char* e = getenv("SB_J3D_DBG"); dbg = (e && e[0] && e[0] != '0') ? 1 : 0; }
        static long n = 0;
        if (dbg && (++n % 2000) == 0)
            fprintf(stderr, "[cam] latch n=%ld gpCamera=%p%s\n", n, (void*)gpCamera,
                    gpCamera ? "" : " (NULL -> no view/proj!)");
    }
    if (!gpCamera) return;
    // GUARD: gpCamera (CPolarSubCamera) is NOT the source of the live camera params here —
    // its fovy/pos/target read 0/garbage (the real gameplay camera transform lives elsewhere;
    // sourcing it is an open RE task). A 0 fovy makes C_MTXPerspective produce inf/NaN and
    // corrupts the view, which is WORSE than leaving the engine's own (valid) j3dSys view. So
    // only publish when the params are sane. TODO: find the real active-camera fovy/view.
    const f32 fovy = gpCamera->getFovy(), aspect = gpCamera->getAspect();
    if (!(fovy > 1.0f && fovy < 179.0f) || !(aspect > 0.01f)) return;

    Vec pos, up, target;
    gpCamera->JSGGetViewPosition(&pos);
    gpCamera->JSGGetViewUpVector(&up);
    gpCamera->JSGGetViewTargetPosition(&target);

    Mtx view;
    C_MTXLookAt(view, &pos, &up, &target);
    j3dSys.setViewMtx(view);

    Mtx44 proj;
    C_MTXPerspective(proj, fovy, aspect, gpCamera->getNear(), gpCamera->getFar());
    sb_gx_latch_proj44(&proj[0][0]);
}
