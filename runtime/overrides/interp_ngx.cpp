// PC-native interp60 — 60 fps frame interpolation on the NGX renderer, NO GX replay.
//
// The game simulates at 30 Hz and renders one frame per tick; the host VI runs 60 fields/s. This
// driver presents TWO ngx frames per game tick — the real frame N and an in-between that is a
// clip-space lerp of N-1↔N — so motion reads at 60 fps. It is entirely object-model: the
// interpolation lives in the ngx present (ngx_present.cpp blends the two published snapshots;
// ngx_j3d_shape.cpp keeps N-1), there is no GX-FIFO capture/replay (that legacy path is now gated
// to SUNBRIGHT_INTERP60_REPLAY for A/B). This is the proof the on-screen image is the native engine:
// the in-between frame exists ONLY in ngx — Dolphin's GX never drew it.
//
// Cadence (mirrors the proven owned-present sequence): on TDisplay::endRendering for a 2-field
// (30 Hz) gameplay frame, take over the present (g_sb_own_present) and:
//   1. render-mode = front; pace one VI field (run the guest endRendering under the interpreter —
//      its EFB copy is discarded, only the field pacing matters); present the REAL frame (ngx front).
//   2. render-mode = blend; steer the display to a distinct XFB address (so the two presents are
//      separate texture-cache keys, not coalesced); pace one more field; present the IN-BETWEEN
//      (ngx renders prev↔front at alpha). The XFB addr is just the present's cache key — ngx
//      substitutes its own blended texture for it.
// 1-field scenes (60 fps menus) present once normally. The blend itself falls back to the real
// frame whenever N-1 and N differ in topology, so interpolation can never corrupt the image.
//
// Gated on SUNBRIGHT_INTERP60 (default ON under ngx — see ngx_interp60_enabled) AND the pure-JIT
// engine + ngx present (it presents ngx's texture, not a guest XFB). Off → the game presents
// normally at 30 fps.

#include "../overrides.h"
#include "../intrinsics.h"
#include "../ngx/ngx_render_data.h"   // ngx_interp60_enabled / ngx_interp_mode / sb_ngx_set_interp_mode

#include <cstdlib>
#include <cstdio>

extern "C" {
extern volatile int g_sb_own_present;        // Present.cpp (fork): runtime owns the present cadence
void sb_present_xfb(unsigned xfb_addr);       // Present.cpp (fork): present an XFB addr now (→ ViSwap)
}

namespace {

constexpr u32 ENDRENDER          = 0x802f80d0u;   // JDrama::TDisplay::endRendering(this)
constexpr u32 VIDEO_SET_NEXT_XFB = 0x802fc99cu;   // JDrama::TVideo::setNextXFB(const void*)

// SUPERSEDED by the fork-level present cadence (ngx_present.cpp detects new-vs-repeat VI fields and
// blends on repeats) — running the guest endRendering twice under the interpreter for the owned-
// present cadence tanked speed to ~0.09× (no fast recomp body under no-recomp). Kept, opt-in only
// (SUNBRIGHT_INTERP60_DRIVE), for reference/A-B; the default native interp needs no guest driver.
bool driver_on() {
    return getenv("SUNBRIGHT_INTERP60_DRIVE") &&
           sunbright_purejit_mode() && ngx_capture_active() && ngx_interp60_enabled();
}

SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_ngx_interp_endRendering, ENDRENDER, driver_on()) {
    const u32 display = cpu.gpr[3];
    const u16 wait = (u16)mem_r16(display + 0x4C);

    // Need a real 2-field (30 Hz) frame AND two topology-matched ngx snapshots to interpolate.
    int nv = 0;  const NgxRenderVertex* fv = ngx_snap_verts(&nv);
    int npv = 0; const NgxRenderVertex* pv = ngx_snap_verts_prev(&npv);
    const bool can_interp = (wait == 2) && fv && nv >= 3 && pv && npv == nv;
    if (!can_interp) {                       // 1-field scene / no prev yet → present once, normally
        g_sb_own_present = 0;
        sb_ngx_set_interp_mode(0);
        call_ppc(cpu, ENDRENDER);
        return;
    }

    g_sb_own_present = 1;
    const u32 orig_fb   = mem_r32(display + 4);
    const u32 alt       = orig_fb ^ 0x00400000u;       // distinct XFB addr for the in-between
    const u32 orig_phys = orig_fb & 0x3FFFFFFFu;
    const u32 alt_phys  = alt     & 0x3FFFFFFFu;
    const u32 video     = mem_r32(display + 0x60);

    // ── REAL field: pace one field, present the real frame N (ngx renders front) ──
    sb_ngx_set_interp_mode(0);
    mem_w16(display + 0x4C, 1);
    cpu.gpr[3] = display; call_ppc(cpu, ENDRENDER);    // guest endRendering under interp: paces 1 field
    sb_present_xfb(orig_phys);

    // ── IN-BETWEEN field: pace one field, present the N-1↔N blend (ngx renders the blend) ──
    cpu.gpr[3] = video; cpu.gpr[4] = alt; call_ppc(cpu, VIDEO_SET_NEXT_XFB);
    sb_ngx_set_interp_mode(1);
    const u32 ob0 = mem_r32(display + 4), ob1 = mem_r32(display + 8);
    mem_w32(display + 4, alt); mem_w32(display + 8, alt);   // steer the copy dest to ALT (cache key)
    mem_w16(display + 0x4C, 1);
    cpu.gpr[3] = display; call_ppc(cpu, ENDRENDER);
    mem_w32(display + 4, ob0); mem_w32(display + 8, ob1);   // restore
    mem_w16(display + 0x4C, wait);
    sb_present_xfb(alt_phys);

    sb_ngx_set_interp_mode(0);   // leave the front mode armed for any non-driver present (e.g. /abshot2)
}

static const bool s_interp_ngx_announce = [] {
    if (driver_on())
        std::fprintf(stderr, "[interp60] PC-native 60fps interpolation ON (ngx object-model blend, "
                             "no GX replay) — SUNBRIGHT_INTERP60=0 to disable\n");
    return true;
}();

}  // namespace
