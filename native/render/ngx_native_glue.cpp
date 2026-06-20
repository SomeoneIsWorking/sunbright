// ngx_native_glue — native-build definitions for the few ngx capture accessors the
// reused shipping ngx code (tev_shader.cpp's /tevshader self-test) references but which
// live in the Dolphin-coupled producer (ngx_j3d_shape.cpp), excluded from sms-render.
//
// The native renderer has no live guest-RAM J3D capture yet (that arrives when the engine
// boots against the platform seams — handoff step 3). Until then there are no captured TEV
// states, so this returns an empty list. The unit tests build NgxTevState explicitly and
// call sb_tev_gen_fragment directly, so they don't depend on a live snapshot.
#include "ngx_render_data.h"

const NgxTevState* ngx_snap_tevstates(int* nstates) {
    if (nstates) *nstates = 0;
    return nullptr;
}
