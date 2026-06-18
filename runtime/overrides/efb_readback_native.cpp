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
extern void mem_w32(u32 ea, u32 v);
extern void mem_w16(u32 ea, u16 v);
extern u8   mem_r8(u32 ea);
extern u16  mem_r16(u32 ea);
extern void sb_run_original_around(CPUState& cpu, u32 addr, void (*after)(u32), u32 cookie);
extern "C" void sb_ngx_efb_request_readback();
extern "C" int  sb_ngx_efb_peek_depth(int gx, int gy, float* out);   // ngx_present.cpp

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

// DIAGNOSTIC: which OTHER EFB-readback consumers fire in the default plaza? GXPeekARGB (Mario color
// sample, always on-screen) + GXCopyTex (mirror/bathwater/mist/manta/heat-haze). Log call counts +
// args so we know where the live verification targets are. Run the original (empty-EFB result for
// now — we only care that they fire + with what rects).
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_gxpeekargb_dbg, 0x8035dcccu, s_efb_dbg) {
    static unsigned long n = 0;
    if ((n++ % 60) == 0)
        fprintf(stderr, "[efb] GXPeekARGB #%lu (x=%u y=%u)\n", n, cpu.gpr[3] & 0xFFFF, cpu.gpr[4] & 0xFFFF);
    sb_run_original_around(cpu, 0x8035dcccu, nullptr, 0);
}
// ── GXCopyTex: serve EFB→texture copies from ngx's scene color (own-the-framebuffer slice 3) ─────
// GXSetTexCopySrc(left,top,wd,ht) + GXSetTexCopyDst(wd,ht,fmt,mip) set the EFB src rect + dst tex
// dims/format; GXCopyTex(dest,clear) copies EFB→dest texture (clear also clears the EFB — irrelevant
// to us). Under ngx present Dolphin's EFB is empty → the copy writes BLACK into the effect's texture.
// We run the original (keeps Dolphin GP state consistent) then OVERWRITE dest with ngx's scene color,
// box-downsampled to the dst dims and GC-tiled in the dst format. Active only under ngx present.
extern "C" int sb_ngx_efb_copy_region(int sx, int sy, int sw, int sh, int dw, int dh, uint32_t* out);
static const bool s_ngx_present = getenv("SUNBRIGHT_NGX_PRESENT") != nullptr;

namespace {
thread_local int g_src_l = 0, g_src_t = 0, g_src_w = 0, g_src_h = 0;   // GXSetTexCopySrc rect
thread_local int g_dst_w = 0, g_dst_h = 0, g_dst_fmt = -1;             // GXSetTexCopyDst dims/fmt
thread_local u32 g_copy_dst_ea = 0;                                    // GXCopyTex dest (this copy)

inline u16 argb_to_rgb565(u32 c) {
    u32 r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
// ngx scene color is opaque → use the RGB5A3 opaque (top-bit-set, RGB555) form. (decode: px_RGB5A3.)
inline u16 argb_to_rgb5a3(u32 c) {
    u32 r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return (u16)(0x8000u | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
}
inline u16 argb_to_tex16(u32 c, int fmt) { return fmt == 5 ? argb_to_rgb5a3(c) : argb_to_rgb565(c); }

// Runs AFTER the original GXCopyTex (via sb_run_original_around): overwrite the dst texture with ngx
// scene color in the captured format. Only RGB565 (fmt=4) is encoded for now (the only live plaza
// copy); other formats leave the original (black) untouched — no regression, just not-yet-served.
void copytex_writeback(u32 /*cookie*/) {
    const int dw = g_dst_w, dh = g_dst_h, fmt = g_dst_fmt;
    if (g_copy_dst_ea == 0 || dw <= 0 || dh <= 0) return;
    if (fmt != 4 && fmt != 5) return;             // GX_TF_RGB565 / RGB5A3 (both 4×4 BE u16) — v1
    const u32 ea = (g_copy_dst_ea & 0x3FFFFFFF) | 0x80000000u;         // phys → cached MEM1 virtual
    static thread_local std::vector<u32> buf;
    buf.resize((size_t)dw * dh);
    int sw = g_src_w > 0 ? g_src_w : 640, sh = g_src_h > 0 ? g_src_h : 448;
    if (!sb_ngx_efb_copy_region(g_src_l, g_src_t, sw, sh, dw, dh, buf.data())) return;  // no frame yet
    // RGB565/RGB5A3 4×4 GC tiling: tile (y,x) row-major; within tile iy-major, each row 4 BE u16. Byte
    // offset = ((y/4)*(dw/4) + x/4)*32 + (y%4)*8 + (x%4)*2. (matches runtime/render/tex_decode.cpp.)
    const int tiles_per_row = dw / 4;
    for (int dy = 0; dy < dh; dy++)
        for (int dx = 0; dx < dw; dx++) {
            u32 tile = (u32)(dy >> 2) * tiles_per_row + (dx >> 2);
            u32 off = tile * 32 + (u32)(dy & 3) * 8 + (u32)(dx & 3) * 2;
            mem_w16(ea + off, argb_to_tex16(buf[(size_t)dy * dw + dx], fmt));   // BE u16
        }
    if (s_efb_dbg) {
        static unsigned long n4 = 0, n5 = 0;
        unsigned long& nn = (fmt == 5) ? n5 : n4;
        if ((nn++ % 60) == 0) {
            int cx = dw / 2, cy = dh / 2;
            u32 toff = ((u32)(cy >> 2) * tiles_per_row + (cx >> 2)) * 32 + (u32)(cy & 3) * 8 + (u32)(cx & 3) * 2;
            u16 stored = mem_r16(ea + toff);          // re-read what we wrote (BE u16 → host)
            u32 srcc = buf[(size_t)cy * dw + cx];
            size_t nz = 0; for (u32 c : buf) if ((c & 0xFFFFFF) != 0) nz++;
            fprintf(stderr, "[efb] CopyTex served ea=%08x %dx%d fmt=%d center src=%06x stored=%04x nz=%zu/%zu\n",
                    ea, dw, dh, fmt, srcc & 0xFFFFFF, stored, nz, buf.size());
        }
    }
}
}  // namespace

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
