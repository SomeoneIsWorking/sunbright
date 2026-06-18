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
extern "C" void ngx_emit_imm_sphere(int numMajor, int numMinor, int r, int g, int b, int a);
extern u8 mem_r8(u32 ea);

namespace {

static const bool s_ngx_present = getenv("SUNBRIGHT_NGX_PRESENT") != nullptr;

// Live COLOR0A0 material colour (GXSetChanMatColor). The GXDrawSphere skybox dome uses PASSCLR with
// the channel disabled → fragment == matColor. GX power-on default is white.
thread_local int g_matcolor0[4] = {255, 255, 255, 255};

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

// GXSetChanMatColor(GXChannelID chan, GXColor* mat_color) @ 0x8035f51c. The SDK passes GXColor BY
// POINTER in r4 (disasm: lbz r,0(r4)/1(r4)/2(r4) = R,G,B; A@3) — see memory gx-color-args-by-pointer.
// Capture the COLOR0/COLOR0A0 slot (matColor[0]) for the immediate-mode sphere's flat colour.
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_imm_chanmatcolor, 0x8035f51cu, s_ngx_present) {
    const u32 chan = cpu.gpr[3];
    const u32 p    = cpu.gpr[4];
    if ((chan == 0u /*GX_COLOR0*/ || chan == 4u /*GX_COLOR0A0*/) && p >= 0x80000000u) {
        g_matcolor0[0] = mem_r8(p + 0); g_matcolor0[1] = mem_r8(p + 1);
        g_matcolor0[2] = mem_r8(p + 2); g_matcolor0[3] = mem_r8(p + 3);
    }
    sb_run_original_around(cpu, 0x8035f51cu, nullptr, 0);
}

// GXDrawSphere(u8 numMajor, u8 numMinor) @ 0x80362268 — the TSky skybox dome. Capture into ngx with
// the live COLOR0A0 matColor, then run the original (keeps Dolphin GP consistent).
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_imm_drawsphere, 0x80362268u, s_ngx_present) {
    const int numMajor = (int)(cpu.gpr[3] & 0xFF);
    const int numMinor = (int)(cpu.gpr[4] & 0xFF);
    ngx_emit_imm_sphere(numMajor, numMinor, g_matcolor0[0], g_matcolor0[1], g_matcolor0[2], g_matcolor0[3]);
    if (getenv("SUNBRIGHT_DBG_EFB")) {
        static unsigned long n = 0;
        if ((n++ % 60) == 0)
            fprintf(stderr, "[imm] GXDrawSphere #%lu (%d,%d) matColor0=(%d,%d,%d,%d)\n",
                    n, numMajor, numMinor, g_matcolor0[0], g_matcolor0[1], g_matcolor0[2], g_matcolor0[3]);
    }
    sb_run_original_around(cpu, 0x80362268u, nullptr, 0);
}

}  // namespace
#endif  // HAVE_DOLPHIN_CORE
