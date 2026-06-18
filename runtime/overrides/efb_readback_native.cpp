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
extern "C" void sb_ngx_efb_request_readback();
extern "C" int  sb_ngx_efb_peek_depth(int gx, int gy, float* out);   // ngx_present.cpp

namespace {
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
