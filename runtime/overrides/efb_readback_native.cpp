// Own-the-framebuffer: serve the guest's EFB-readback from ngx's rendered scene, not Dolphin's EFB
// (which is empty under ngx present — ngx skips the guest GX draws). Slice 1: GXPeekZ.
//
// Under ngx present the guest's TSunModel::getZBufValue (sun-occlusion lens flare) peeks Z at ~17
// points around the sun and counts how many read FAR (0xFFFFFF = nothing drawn = sun visible) vs
// occluded → glow strength. Dolphin's GXPeekZ reads the empty EFB → wrong occlusion. We replace it
// with ngx's last-rendered scene DEPTH (read back to CPU in ngx_present; 1-frame lag, fine here).
// See debug_journal/2026-06-18_own_gpu_framebuffer_frontier.md.
#include "../overrides.h"

#ifdef HAVE_DOLPHIN_CORE
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../render/ngx_efb_copy.h"   // pure encode + GC tiling (unit-tested in render_test)
extern void mem_w32(u32 ea, u32 v);
extern void mem_w16(u32 ea, u16 v);
extern u8   mem_r8(u32 ea);
extern u16  mem_r16(u32 ea);
extern void sb_run_original_around(CPUState& cpu, u32 addr, void (*after)(u32), u32 cookie);
extern "C" void sb_ngx_efb_request_readback();
extern "C" int  sb_ngx_efb_peek_depth(int gx, int gy, float* out);   // ngx_present.cpp
extern "C" int  sb_ngx_imm_occ_depth(float* out);                    // ngx_j3d_shape.cpp (cube front depth)
#include "../ngx/ngx_imm_geom.h"                                     // ngx_imm::imm_occluded (unit-tested)

namespace {

// ── DIAGNOSTIC (SUNBRIGHT_DBG_EFB) — does the sun-occlusion drawsync callback fire under ngx? ─────
// GXPeekZ logged 0 calls in fastboot Delfino. It is reached only via TSunMgr::drawSyncCallback
// (0x8002e270), which the TDrawSyncManager token mechanism dispatches. These observers wrap (run-
// original-around) perform + drawSyncCallback to tell which link of the chain is dead:
//   perform fires, drawSyncCallback doesn't → drawsync-token delivery is broken under ngx (the blocker)
//   perform doesn't fire                    → TSunMgr not alive in this scene / camera
//   both fire but GXPeekZ still 0           → unk14==0 (no sun model loaded this scene)
static const bool s_efb_dbg = getenv("SUNBRIGHT_DBG_EFB") != nullptr;

// void TSunMgr::perform(u32, TGraphics*) — per-frame view-tree tick; proves TSunMgr is in the scene.
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_sunmgr_perform, 0x8002e2d0u, s_efb_dbg) {
    static unsigned long n = 0;
    if ((n++ % 120) == 0) {
        const u32 self = cpu.gpr[3];
        fprintf(stderr, "[efb] TSunMgr::perform #%lu  this=%08x unk14=%u unk15=%u\n",
                n, self, self >= 0x80000000u ? mem_r8(self + 0x14) : 0,
                self >= 0x80000000u ? mem_r8(self + 0x15) : 0);
    }
    sb_run_original_around(cpu, 0x8002e2d0u, nullptr, 0);
}

// void TSunMgr::drawSyncCallback(u16) — fires only if the drawsync token reaches the dispatcher;
// calls TSunModel::getZBufValue → GXPeekZ when unk14 is set.
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_sunmgr_dsc, 0x8002e270u, s_efb_dbg) {
    const u32 self = cpu.gpr[3];
    static unsigned long n = 0;
    fprintf(stderr, "[efb] TSunMgr::drawSyncCallback #%lu  this=%08x unk14=%u tok=%u\n",
            ++n, self, self >= 0x80000000u ? mem_r8(self + 0x14) : 0, cpu.gpr[4] & 0xFFFF);
    sb_run_original_around(cpu, 0x8002e270u, nullptr, 0);
}

// void TSunModel::getZBufValue() — gpr[3]=this=gpSunModel. GXPeekZ is skipped per-point when Mario
// is indoor OR the sample point projects off-screen (unkB4[i] == (-1,-1)). Log the 17 screen
// positions (s16 pairs at this+0xB4) to see WHY GXPeekZ never fires.
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_getzbuf, 0x8002ea70u, s_efb_dbg) {
    const u32 self = cpu.gpr[3];
    static unsigned long n = 0;
    if (self >= 0x80000000u && (n++ % 120) == 0) {
        char buf[512]; int p = 0, valid = 0;
        for (int i = 0; i < 17; ++i) {
            s16 x = (s16)mem_r16(self + 0xB4 + i*4), y = (s16)mem_r16(self + 0xB6 + i*4);
            if (x != -1 && y != -1) valid++;
            if (i < 6) p += snprintf(buf+p, sizeof(buf)-p, "(%d,%d)", x, y);
        }
        fprintf(stderr, "[efb] getZBufValue #%lu  this=%08x valid_pts=%d/17  first6=%s\n",
                n, self, valid, buf);
    }
    sb_run_original_around(cpu, 0x8002ea70u, nullptr, 0);
}

// GXPeekARGB(u16 x, u16 y, u32* color) @ 0x8035dccc — the Mario occlusion probe
// (TMario::drawSyncCallback tests (color & 0xff000000) == 0x10000000 ⇒ NOT occluded). The game's
// trick is a z-tested cube that stamps framebuffer alpha 0x10 where Mario is in front of the scene;
// ngx can't serve that mid-frame read-after-write from one end-of-frame readback (root cause pinned
// 2026-06-19, see debug_journal/2026-06-18_immediate_mode_gx_geometry.md). We answer the query
// DIRECTLY from depth instead: Mario is occluded iff the ngx scene depth at his pixel is nearer than
// the occlusion cube's front face (the cube encloses Mario → his body never self-occludes). Then we
// synthesize the GXColor the game expects: alpha 0x10 when visible, 0 when occluded. Only the alpha
// byte is read by the game. Under the Dolphin-GX baseline (no ngx present) the real GXPeekARGB runs.
static const bool s_ngx_present_occ = getenv("SUNBRIGHT_NGX_PRESENT") != nullptr;
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_gxpeekargb, 0x8035dcccu, s_ngx_present_occ) {
    const int x = (int)(cpu.gpr[3] & 0xFFFF), y = (int)(cpu.gpr[4] & 0xFFFF);
    const u32 outp = cpu.gpr[5];
    sb_ngx_efb_request_readback();                 // keep the depth readback armed
    float scene_depth = -1.f, cube_front = 1e9f;
    const bool have_d = sb_ngx_efb_peek_depth(x, y, &scene_depth);
    const bool have_c = sb_ngx_imm_occ_depth(&cube_front);
    if (outp >= 0x80000000u && have_d && have_c) {
        static float eps = []{ const char* e = getenv("SUNBRIGHT_NGX_OCC_EPS"); return e ? (float)atof(e) : 0.0f; }();
        const bool occluded = ngx_imm::imm_occluded(scene_depth, cube_front, eps);
        mem_w32(outp, occluded ? 0x00000000u : 0x10000000u);   // alpha in the high byte (game reads only that)
        if (s_efb_dbg) { static unsigned long n = 0;
            if ((n++ % 60) == 0) fprintf(stderr, "[efb] GXPeekARGB(occ) #%lu (x=%d y=%d) scene_d=%.5f cube_front=%.5f -> occluded=%d\n",
                n, x, y, scene_depth, cube_front, occluded); }
        return;
    }
    sb_run_original_around(cpu, 0x8035dcccu, nullptr, 0);   // no readback/cube yet → original
}
// ── GXCopyTex: serve EFB→texture copies from ngx's scene color (own-the-framebuffer slice 3) ─────
// GXSetTexCopySrc(left,top,wd,ht) + GXSetTexCopyDst(wd,ht,fmt,mip) set the EFB src rect + dst tex
// dims/format; GXCopyTex(dest,clear) copies EFB→dest texture (clear also clears the EFB — irrelevant
// to us). Under ngx present Dolphin's EFB is empty → the copy writes BLACK into the effect's texture.
// We run the original (keeps Dolphin GP state consistent) then OVERWRITE dest with ngx's scene color,
// box-downsampled to the dst dims and GC-tiled in the dst format. Active only under ngx present.
extern "C" int sb_ngx_efb_copy_region(int sx, int sy, int sw, int sh, int dw, int dh, uint32_t* out);
extern "C" void sb_ngx_efb_invalidate_tex(uint32_t ea);
extern "C" void sb_ngx_efb_store_copy(uint32_t ea, int w, int h, const uint32_t* argb);
static const bool s_ngx_present = getenv("SUNBRIGHT_NGX_PRESENT") != nullptr;

namespace {
thread_local int g_src_l = 0, g_src_t = 0, g_src_w = 0, g_src_h = 0;   // GXSetTexCopySrc rect
thread_local int g_dst_w = 0, g_dst_h = 0, g_dst_fmt = -1;             // GXSetTexCopyDst dims/fmt
thread_local u32 g_copy_dst_ea = 0;                                    // GXCopyTex dest (this copy)

using ngx_efb::argb_to_tex16;   // pure encode lives in render/ngx_efb_copy.h (unit-tested)

// Runs AFTER the original GXCopyTex (via sb_run_original_around): overwrite the dst texture with ngx
// scene color in the captured format. Only RGB565 (fmt=4) is encoded for now (the only live plaza
// copy); other formats leave the original (black) untouched — no regression, just not-yet-served.
extern "C" volatile int g_sb_efb_copy_on = 1;   // /efbcopy?on=N — A/B toggle for the EFB-copy writeback

void copytex_writeback(u32 /*cookie*/) {
    const int dw = g_dst_w, dh = g_dst_h, fmt = g_dst_fmt;
    if (g_copy_dst_ea == 0 || dw <= 0 || dh <= 0) { if (s_efb_dbg){static unsigned long z=0; if((z++%240)==0) fprintf(stderr,"[efb-wb] skip: ea/dim ea=%08x %dx%d\n",g_copy_dst_ea,dw,dh);} return; }
    if (fmt != 4 && fmt != 5) { if (s_efb_dbg){static unsigned long z=0; if((z++%240)==0) fprintf(stderr,"[efb-wb] skip: fmt=%d (%dx%d)\n",fmt,dw,dh);} return; }
    const u32 ea = (g_copy_dst_ea & 0x3FFFFFFF) | 0x80000000u;         // phys → cached MEM1 virtual
    sb_ngx_efb_invalidate_tex(ea);     // always re-decode this tex (ON: ngx scene below; OFF: original's black)
    if (!g_sb_efb_copy_on) return;                // A/B off → effect samples the original's (black) copy
    static thread_local std::vector<u32> buf;
    buf.resize((size_t)dw * dh);
    int sw = g_src_w > 0 ? g_src_w : 640, sh = g_src_h > 0 ? g_src_h : 448;
    if (!sb_ngx_efb_copy_region(g_src_l, g_src_t, sw, sh, dw, dh, buf.data())) { if (s_efb_dbg){static unsigned long z=0; if((z++%120)==0) fprintf(stderr,"[efb-wb] NO FRAME (efb_copy_region failed) ea=%08x %dx%d fmt=%d\n",ea,dw,dh,fmt);} return; }  // no frame yet
    // Store in ngx's side buffer (texture_for reads THIS, immune to Dolphin's async EFB→RAM stomp).
    sb_ngx_efb_store_copy(ea, dw, dh, buf.data());
    // RGB565/RGB5A3 4×4 GC tiling: tile (y,x) row-major; within tile iy-major, each row 4 BE u16. Byte
    // offset = ((y/4)*(dw/4) + x/4)*32 + (y%4)*8 + (x%4)*2. (matches runtime/render/tex_decode.cpp.)
    for (int dy = 0; dy < dh; dy++)
        for (int dx = 0; dx < dw; dx++)
            mem_w16(ea + ngx_efb::tile_offset16(dx, dy, dw),
                    argb_to_tex16(buf[(size_t)dy * dw + dx], fmt));   // BE u16
    if (s_efb_dbg) {
        static unsigned long n4 = 0, n5 = 0;
        unsigned long& nn = (fmt == 5) ? n5 : n4;
        if ((nn++ % 60) == 0) {
            int cx = dw / 2, cy = dh / 2;
            u16 stored = mem_r16(ea + ngx_efb::tile_offset16(cx, cy, dw));   // re-read what we wrote (BE u16 → host)
            u32 srcc = buf[(size_t)cy * dw + cx];
            size_t nz = 0; for (u32 c : buf) if ((c & 0xFFFFFF) != 0) nz++;
            fprintf(stderr, "[efb] CopyTex served ea=%08x %dx%d fmt=%d center src=%06x stored=%04x nz=%zu/%zu\n",
                    ea, dw, dh, fmt, srcc & 0xFFFFFF, stored, nz, buf.size());
        }
    }
}
}  // namespace

// GXDrawSphere (0x80362268) is now CAPTURED into ngx in imm_geom_native.cpp (the TSky skybox dome),
// alongside GXDrawCube. (The old diagnostic stub here was removed to avoid a duplicate override.)

SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_gxsetcopysrc, 0x8035e388u, s_ngx_present) {
    g_src_l = cpu.gpr[3] & 0xFFFF; g_src_t = cpu.gpr[4] & 0xFFFF;
    g_src_w = cpu.gpr[5] & 0xFFFF; g_src_h = cpu.gpr[6] & 0xFFFF;
    sb_run_original_around(cpu, 0x8035e388u, nullptr, 0);
}
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_gxsetcopydst, 0x8035e48cu, s_ngx_present) {
    g_dst_w = cpu.gpr[3] & 0xFFFF; g_dst_h = cpu.gpr[4] & 0xFFFF; g_dst_fmt = (int)cpu.gpr[5];
    sb_run_original_around(cpu, 0x8035e48cu, nullptr, 0);
}
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_gxcopytex, 0x8035ee5cu, s_ngx_present) {
    g_copy_dst_ea = cpu.gpr[3];
    if (s_efb_dbg) {
        static unsigned long n = 0;
        if ((n++ % 240) == 0)
            fprintf(stderr, "[efb] GXCopyTex #%lu dst=%08x clear=%u src[%d,%d,%d,%d] dstTex[%dx%d fmt=%d]%s\n",
                    n, cpu.gpr[3], cpu.gpr[4], g_src_l, g_src_t, g_src_w, g_src_h,
                    g_dst_w, g_dst_h, g_dst_fmt, (g_dst_fmt == 4 || g_dst_fmt == 5) ? " ←served" : "");
    }
    sb_run_original_around(cpu, 0x8035ee5cu, &copytex_writeback, 0);
}

// void GXPeekZ(u16 x, u16 y, u32* z) — read the EFB Z at (x,y). r3=x, r4=y, r5=&z.
SUNBRIGHT_OVERRIDE_NATIVE(ov_gxpeekz, 0x8035dcf0u) {
    const u32 x = cpu.gpr[3] & 0xFFFF, y = cpu.gpr[4] & 0xFFFF, zp = cpu.gpr[5];
    sb_ngx_efb_request_readback();            // arm the depth readback for upcoming frames
    u32 z = 0x00FFFFFFu;                       // default FAR (sun visible) until a frame is published
    float d;
    if (sb_ngx_efb_peek_depth((int)x, (int)y, &d)) {
        if (d < 0.0f) d = 0.0f; else if (d > 1.0f) d = 1.0f;
        z = (u32)(d * 16777215.0f + 0.5f);     // [0,1] → 24-bit GC Z (1.0 = 0xFFFFFF = far/empty)
    }
    if (zp >= 0x80000000u) mem_w32(zp, z);
    if (getenv("SUNBRIGHT_DBG_EFB")) {
        static unsigned long n = 0; static u32 zmin = 0xFFFFFFFF, zmax = 0; static int nfar = 0, nocc = 0;
        if (z < zmin) zmin = z; if (z > zmax) zmax = z;
        if (z >= 0x00FFFFFFu) nfar++; else nocc++;
        if ((n++ % 120) == 0)
            fprintf(stderr, "[efb] GXPeekZ #%lu (x=%u y=%u) z=%06x  [zmin=%06x zmax=%06x far=%d occ=%d]\n",
                    n, x, y, z, zmin, zmax, nfar, nocc);
    }
    // GXPeekZ is void; nothing to return in r3.
}
}  // namespace

#endif  // HAVE_DOLPHIN_CORE
