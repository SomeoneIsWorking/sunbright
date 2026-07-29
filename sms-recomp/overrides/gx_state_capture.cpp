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
extern "C" void func_80360160(CPUState&);   // GXLoadTexObj(GXTexObj*, GXTexMapID)

namespace {

// Power-on default: test on, LEQUAL, write on. GX's own reset state, so a shape drawn before any
// material has set a z-mode is treated the way the hardware would treat it.
SbrDepthState g_zmode{1, 3, 1, 0, 1, 0, 1, 1};   // blend NONE, src ONE, dst ZERO, writes on

// The projection currently loaded, whatever its type. A drawable must be projected with the matrix
// that was actually current when the game drew it, not with the scene's main perspective.
float g_proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
bool  g_proj2d = false;

// The texture bound to TEXMAP0, decoded straight out of the guest GXTexObj. The SDK type is opaque
// (u32[8]), but the hardware encoding is fixed: image0 at +8 carries width-1, height-1 and format;
// image3 at +12 carries the data address >> 5.
SbrTexture g_tex0{};

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

// GXLoadTexObj(GXTexObj* obj, GXTexMapID id)
void ov_gx_load_tex_obj(CPUState& cpu) {
    const u32 obj = cpu.gpr[3];
    const u32 id  = cpu.gpr[4];
    if (id == 0 && sb_ram_fast(obj) != nullptr) {
        const u32 image0 = sb_r32(obj + 8);
        const u32 image3 = sb_r32(obj + 12);
        g_tex0.width  = (image0 & 0x3FF) + 1;
        g_tex0.height = ((image0 >> 10) & 0x3FF) + 1;
        g_tex0.format = (image0 >> 20) & 0xF;
        // image3 holds a PHYSICAL address >> 5. Guest pointers carry the 0x80000000 cached base;
        // without it every lookup misses RAM and reports as an undecodable format.
        g_tex0.addr   = ((image3 & 0x00FFFFFF) << 5) | 0x80000000u;
        // TLUT base for the colour-indexed formats: GXTexObj word 5 holds the TLUT physical
        // address >> 5 once GXInitTexObjTlut has run.
        g_tex0.tlut   = ((sb_r32(obj + 20) & 0x00FFFFFF) << 5) | 0x80000000u;
    }
    func_80360160(cpu);
}

} // namespace

SbrDepthState sbr_gx_current_zmode() { return g_zmode; }
SbrTexture    sbr_gx_current_texture() { return g_tex0; }

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

SB_OVERRIDE(0x80360160u, ov_gx_load_tex_obj, "GXLoadTexObj",
            "native render: mirror the bound TEXMAP0 texture (always runs the real body)")
