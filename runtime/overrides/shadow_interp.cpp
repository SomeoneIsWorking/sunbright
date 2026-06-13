// 60fps interpolation — Mario's REAL cast shadow (TMBindShadowManager, the bind-shadow system).
//
// The cast shadow is an EFB DESTINATION-ALPHA STENCIL SHADOW VOLUME (RE'd from disasm — there is
// NO GXCopyTex / texture in the chain; docs/re_notes/cast_shadow_alpha_chain.md):
//   Pass 1 (drawShadowVolume 0x802305dc): render the volume with color-update off / alpha-update on
//     / dst-alpha / Z-test-no-write -> stamps a per-pixel mask into the EFB ALPHA channel.
//   Pass 2: a dark screen-space GXQUAD with dst-alpha test -> darkens the EFB only where the volume
//     set alpha. The dynamic resource is the EFB alpha, written+consumed WITHIN the frame, BEFORE
//     the EFB->XFB copy.
//   TMBindShadowManager::perform(param,gfx) @ 0x80231108: &0x4 calcVtx 0x8022e0cc (build geometry
//     into *(this+0x1c), count this+0x20, from per-frame requests); &0x8 drawShadowGD 0x8022fa40 /
//     drawShadow 0x8022f014 (the stencil+composite); &0x20000000 finalize (zero counts).
//
// THE BUG was a PHASE/draw-order bug, not geometry. The shadow runs in the +0x20 silhouette perform
// list: AFTER +0x1C (3D scene fills EFB) and BEFORE +0x24 GXPost (reprograms masks + GXCopyDisp
// EFB->XFB). The earlier code SUPPRESSED the in-between +0x20 perform and replayed the shadow AFTER
// the whole draw-list loop — i.e. AFTER +0x24 already copied the EFB out — so the stencil+composite
// ran out of phase and never landed in the presented field = the on/off blink. (This is why every
// geometry-restore / draw-path attempt failed: geometry was never the missing ingredient.)
//
// THE FIX: draw the shadow IN PHASE at the +0x20 slot on the in-between (the kDrawLists re-issue
// runs +0x20 before +0x24), with a DRAW-ONLY mask: keep &0x8 (draw the stencil+composite), drop
// &0x4 (calcVtx would rebuild the request-driven geometry to EMPTY at 60Hz) and &0x20000000
// (finalize would re-zero the counts); restore the draw count finalize zeroed on the real field
// (mgr+0x20) from the real-field capture so drawShadowGD loops the intact geometry. The geometry
// buffer *(this+0x1c) is a stable heap allocation (load 0x80231288) so it survives intact from the
// real field; only the count word was zeroed. No suppression, no trailing replay, no rebuild.
#include "../overrides.h"
#include "../intrinsics.h"

extern bool g_interp60_in_redraw;   // interp_redraw.cpp — true while re-issuing the in-between draw
bool sunbright_interp60();          // interp60.h

#ifdef HAVE_DOLPHIN_CORE
namespace {
constexpr u32 BINDSHADOW_MGR_PTR = 0x8040E0C0u;  // -> TMBindShadowManager* (US, [r13-0x6100])
constexpr u32 OFF_DRAW_COUNT = 0x20u;            // built draw-array entry count
constexpr u32 BUILD_BIT      = 0x4u;             // calcVtx (rebuild) phase
constexpr u32 DRAW_BIT       = 0x8u;             // draw (stencil+composite) phase
constexpr u32 FINALIZE_BIT   = 0x20000000u;      // finalize (zero counts) phase

u32 g_draw_count = 0;                            // last real field's built draw count

inline bool valid_ram(u32 a) { return a >= 0x80000000u && a < 0x81800000u; }
inline u32  shadow_mgr() { const u32 m = MEM_R32(BINDSHADOW_MGR_PTR); return valid_ram(m) ? m : 0; }

// Real field, at the draw seam (calcVtx ran, finalize hasn't): the count is the built value.
inline void capture_real() {
    if (!sunbright_interp60() || g_interp60_in_redraw) return;
    const u32 m = shadow_mgr();
    if (!m) return;
    const u32 c = MEM_R32(m + OFF_DRAW_COUNT);
    if (c <= 0x200u) g_draw_count = c;
}
}  // namespace

extern "C" void func_8022f014(CPUState&);  // TMBindShadowManager::drawShadow
extern "C" void func_8022fa40(CPUState&);  // TMBindShadowManager::drawShadowGD
extern "C" void func_80231108(CPUState&);  // TMBindShadowManager::perform

unsigned long g_shadow_interp_real = 0, g_shadow_interp_redraw = 0;   // diag (probe)

SUNBRIGHT_OVERRIDE(ov_bindshadow_draw,   0x8022f014u) { capture_real(); func_8022f014(cpu); }
SUNBRIGHT_OVERRIDE(ov_bindshadow_drawgd, 0x8022fa40u) { capture_real(); func_8022fa40(cpu); }

// In-phase in-between draw: the +0x20 list re-issue calls this BEFORE +0x24's EFB->XFB copy.
// Restore the count and run DRAW-ONLY (keep &8, drop &4 rebuild + finalize) over the intact geometry.
SUNBRIGHT_OVERRIDE(ov_bindshadow_perform, 0x80231108u) {
    if (sunbright_interp60() && g_interp60_in_redraw) {
        const u32 m = shadow_mgr();
        if (!m || !g_draw_count) return;     // nothing captured: don't let &4 rebuild empty geometry
        MEM_W32(m + OFF_DRAW_COUNT, g_draw_count);
        cpu.gpr[4] = (cpu.gpr[4] & ~(BUILD_BIT | FINALIZE_BIT)) | DRAW_BIT;
        g_shadow_interp_redraw++;
    }
    func_80231108(cpu);
}
#endif
