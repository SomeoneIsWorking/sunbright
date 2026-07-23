// gx_state_capture.cpp — mirror the GX pixel state the native renderer needs.
//
// The native path draws J3D shapes directly (overrides/j3d_capture.cpp) rather than interpreting
// the GX command stream, so it does not automatically see the pixel-pipeline state the material
// set up before the draw. J3DMaterial::load issues that state through the ordinary GX entry points
// immediately before J3DShape::draw, so mirroring the entry points gives the CURRENT state at draw
// time — which is exactly what the shape is drawn with.
//
// These overrides ALWAYS run the real body: aurora must keep seeing identical state, because it is
// the parity oracle the native path is scored against.

#include "overrides.h"

#include "../runtime/native_render.h"

#include <intrinsics.h>

extern "C" void func_80361f54(CPUState&);   // GXSetZMode
extern "C" void func_80361dd0(CPUState&);   // GXSetBlendMode

namespace {

// Power-on default: test on, LEQUAL, write on. GX's own reset state, so a shape drawn before any
// material has set a z-mode is treated the way the hardware would treat it.
SbrDepthState g_zmode{1, 3, 1, 0, 1, 0};   // blend NONE, src ONE, dst ZERO

// The projection currently loaded, whatever its type. A drawable must be projected with the matrix
// that was actually current when the game drew it, not with the scene's main perspective.
float g_proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
bool  g_proj2d = false;

// GXSetZMode(GXBool compare_enable, GXCompare func, GXBool update_enable)
void ov_gx_set_zmode(CPUState& cpu) {
    g_zmode.test  = (uint8_t)(cpu.gpr[3] != 0);
    g_zmode.func  = (uint8_t)(cpu.gpr[4] & 7);
    g_zmode.write = (uint8_t)(cpu.gpr[5] != 0);
    func_80361f54(cpu);
}

// GXSetBlendMode(GXBlendMode type, GXBlendFactor src, GXBlendFactor dst, GXLogicOp op)
void ov_gx_set_blend_mode(CPUState& cpu) {
    g_zmode.blend  = (uint8_t)(cpu.gpr[3] & 3);
    g_zmode.srcFac = (uint8_t)(cpu.gpr[4] & 7);
    g_zmode.dstFac = (uint8_t)(cpu.gpr[5] & 7);
    func_80361dd0(cpu);
}

} // namespace

SbrDepthState sbr_gx_current_zmode() { return g_zmode; }

void sbr_gx_set_projection(const float m[16], bool is2d) {
    __builtin_memcpy(g_proj, m, sizeof g_proj);
    g_proj2d = is2d;
}
const float* sbr_gx_current_projection(bool* is2d) {
    if (is2d != nullptr) *is2d = g_proj2d;
    return g_proj;
}

SB_OVERRIDE(0x80361f54u, ov_gx_set_zmode, "GXSetZMode",
            "native render: mirror per-material depth state (always runs the real body)")

SB_OVERRIDE(0x80361dd0u, ov_gx_set_blend_mode, "GXSetBlendMode",
            "native render: mirror per-material blend state (always runs the real body)")
