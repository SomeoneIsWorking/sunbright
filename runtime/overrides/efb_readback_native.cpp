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
extern void mem_w32(u32 ea, u32 v);
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
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_gxcopytex_dbg, 0x8035ee5cu, s_efb_dbg) {
    static unsigned long n = 0, nclear0 = 0;
    if ((cpu.gpr[4] & 1) == 0) nclear0++;          // clear=0 = the EFB→texture readbacks (the effects)
    if ((n++ % 240) == 0)
        fprintf(stderr, "[efb] GXCopyTex #%lu (dst=%08x clear=%u)  readbacks(clear=0)=%lu\n",
                n, cpu.gpr[3], cpu.gpr[4], nclear0);
    sb_run_original_around(cpu, 0x8035ee5cu, nullptr, 0);
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
