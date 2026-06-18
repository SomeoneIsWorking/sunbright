// Immediate-mode GX geometry capture for ngx — GXDrawCube (the Mario occlusion probe).
//
// ngx renders from the J3D object model; the game's immediate-mode GX primitives (GXDraw.c:
// GXDrawCube / GXDrawSphere) have no J3D object, so the J3DShape capture path misses them. The
// per-frame GXDrawCube in TMario::draw (MarioMain.cpp ~195) is the occlusion probe: a unit cube at
// Mario, drawn with GXSetColorUpdate(FALSE)+GXSetAlphaUpdate(TRUE)+GXSetDstAlpha(ENABLE,0x10) and a
// z-test, so it stamps a constant framebuffer ALPHA 0x10 wherever the cube is visible (in front of
// scene geometry). TMario::drawSyncCallback then GXPeekARGB's Mario's screen centre: alpha==0x10 ⇒
// not occluded. Under ngx that cube was never drawn → the EFB alpha is never 0x10 → occlusion broken.
//
// Here we tap the live GX write-mask state (GXSetColorUpdate/AlphaUpdate/DstAlpha) and, at GXDrawCube,
// hand the masks to ngx (ngx_emit_imm_cube, ngx_j3d_shape.cpp) which builds the cube in clip space
// from the current GX_PNMTX0 + projection and emits it into the same batch pipeline — colour writes
// masked OFF, the const dst-alpha riding the PASSCLR vertex alpha, depth-tested against the live
// scene. So it writes alpha 0x10 exactly where the GC would, WITHOUT painting a visible box.
//
// Active only under ngx present (else Dolphin's GX draws the real cube into the real EFB). We still
// run the original each time (sb_run_original_around) so Dolphin GP state stays consistent.

#include "../overrides.h"

#ifdef HAVE_DOLPHIN_CORE
#include <cstdio>
#include <cstdlib>

extern void sb_run_original_around(CPUState& cpu, u32 addr, void (*after)(u32), u32 cookie);
extern "C" void ngx_emit_imm_cube(int color_off, int alpha_off, int dst_alpha_en, int dst_alpha_val);

namespace {

static const bool s_ngx_present = getenv("SUNBRIGHT_NGX_PRESENT") != nullptr;

// Live GX framebuffer write-mask state (the box draw sets these immediately before GXDrawCube).
// GX power-on defaults: colour update ON, alpha update OFF, dst-alpha disabled.
thread_local int g_color_update = 1;      // GXSetColorUpdate(enable)
thread_local int g_alpha_update = 0;      // GXSetAlphaUpdate(enable)
thread_local int g_dst_alpha_en = 0;      // GXSetDstAlpha(enable, val)
thread_local int g_dst_alpha_val = 0;

SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_imm_colorupdate, 0x80361ed4u, s_ngx_present) {   // GXSetColorUpdate(GXBool)
    g_color_update = (int)(cpu.gpr[3] & 0xFF) ? 1 : 0;
    sb_run_original_around(cpu, 0x80361ed4u, nullptr, 0);
}
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_imm_alphaupdate, 0x80361f14u, s_ngx_present) {   // GXSetAlphaUpdate(GXBool)
    g_alpha_update = (int)(cpu.gpr[3] & 0xFF) ? 1 : 0;
    sb_run_original_around(cpu, 0x80361f14u, nullptr, 0);
}
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_imm_dstalpha, 0x8036215cu, s_ngx_present) {      // GXSetDstAlpha(GXBool, u8)
    g_dst_alpha_en  = (int)(cpu.gpr[3] & 0xFF) ? 1 : 0;
    g_dst_alpha_val = (int)(cpu.gpr[4] & 0xFF);
    sb_run_original_around(cpu, 0x8036215cu, nullptr, 0);
}

// GXDrawCube() @ 0x803627fc. Capture into ngx, then run the original (keeps Dolphin GP consistent).
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_imm_drawcube, 0x803627fcu, s_ngx_present) {
    // First slice: only the alpha-only OCCLUSION PROBE (colour writes OFF). The visible silhouette
    // path (MarioMain.cpp ~219, colour ON, dst-alpha blend, a non-white matColor) needs matColor +
    // blend-mode capture and is left to a follow-up — un-emitted, exactly as before (no regression).
    const int color_off = !g_color_update;
    if (color_off)
        ngx_emit_imm_cube(color_off, !g_alpha_update, g_dst_alpha_en, g_dst_alpha_val);
    if (getenv("SUNBRIGHT_DBG_EFB")) {
        static unsigned long n = 0;
        if ((n++ % 60) == 0)
            fprintf(stderr, "[imm] GXDrawCube #%lu colorOff=%d alphaUpd=%d dstA=%d/%#x emit=%d\n",
                    n, color_off, g_alpha_update, g_dst_alpha_en, g_dst_alpha_val, color_off);
    }
    sb_run_original_around(cpu, 0x803627fcu, nullptr, 0);
}

}  // namespace
#endif  // HAVE_DOLPHIN_CORE
