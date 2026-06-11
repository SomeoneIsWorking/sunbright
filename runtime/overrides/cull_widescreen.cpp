// Widescreen 3D culling fix (GMSE01).
//
// The game's actor culling (livemanager/NPC/animal managers → DrawUtil
// ViewFrustumClipCheck) tests against a frustum set by
// SetViewFrustumClipCheckPerspective(fovy, aspect, near, far) — with the camera's 4:3
// aspect. Our widescreen hack widens the GPU projection only, so actors inside the
// extra 16:9 side thirds were still culled (pop-in/out at the edges).
//
// Fix: wrap the setter and scale the aspect by (16/9)/(4/3) = 4/3 before the
// recompiled body runs. The body itself stays the game's own code (it computes the
// near-plane half-extents and forwards to SetViewFrustumClipCheck, with caching on
// the passed values — scaling the input keeps the cache key consistent).
// Disabled when SUNBRIGHT_WIDESCREEN=0.
//
//   0x802260cc SetViewFrustumClipCheckPerspective(f32 fovy/f1, f32 aspect/f2,
//                                                 f32 near/f3, f32 far/f4)

#include "../overrides.h"
#include "../intrinsics.h"

#include <cstdlib>

extern "C" void func_802260cc(CPUState&);

namespace {

SUNBRIGHT_OVERRIDE(ov_SetViewFrustumClipCheckPerspective, 0x802260ccu) {
    static const char* e = getenv("SUNBRIGHT_WIDESCREEN");
    static const bool on = !(e && e[0] == '0');
    if (on)
        cpu.fpr[2].ps0 *= (4.0 / 3.0);   // 4:3 camera aspect → 16:9 cull width
    func_802260cc(cpu);
}

} // namespace
