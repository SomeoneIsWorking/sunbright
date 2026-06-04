// Native HUD ownership — Super Mario Sunshine in-game HUD (TGCConsole2).
//
// We OWN the in-game HUD's widescreen layout. The game authors the HUD in 4:3 (640×448),
// corner-anchored: the coin/shine/fruit counters hug the top-left, the lives counter sits
// top-centre, the FLUDD water gauge hugs the bottom-right, the health "sun" is bottom-centre.
//
// The 4:3 EFB is presented at 16:9. ov_gx_projection (scene_render.cpp) pre-squeezes ALL 2D by
// SUNBRIGHT_WS_SCALE (0.75=(4:3)/(16:9)) so the EFB renders anamorphically and the 16:9 present
// restores correct aspect. But that squeeze also pulls the corner-anchored HUD *inward* — it lands
// centred in the old 4:3 safe area instead of hugging the real 16:9 screen edges. So a counter that
// belongs at the top-left edge ends up inset by 12.5% of the screen width, and the water gauge
// floats in from the bottom-right. We fix that here, per element.
//
// HOW WE OWN IT — by element IDENTITY, never a temporal flag. Every HUD picture is drawn through
//   J2DPicture::drawFullSet(this, x, y, w, h, ...)            (USA/GMSE01 0x802cc838)
// where the destination rect is in the ARGS (r4=x r5=y r6=w r7=h, 640×448 space). So we own each
// element's position by rewriting r4 before the draw. The squeeze already gives correct aspect, so
// we only TRANSLATE x toward the element's 16:9 edge — width/height/Y are left alone.
//
// We classify each element by its .blo NAME (the J2DPane fourCC at this+0x10). We deliberately do
// NOT use a "we're inside the HUD" flag: the scene draw is tail-recursive, so such a flag leaks
// across the whole frame (it burned us twice — it bled the HUD transform onto the menus). The
// file-select menu ALSO draws through drawFullSet, with names s_0a / .s_1 / n_0a / shn0 / yaji /
// .x_0. We match ONLY the HUD's exact role-gated names (cluster letter ∈ {s,d,c,m,w} + a HUD role
// suffix ba|ic|tx|x|n<d>|t<d>, or go<NN>); the menu's _0<x> roles never match → menus are NEVER
// touched. No flag, no leak.
//
// The HUD element map (verified live against a Delfino gameplay save, scratch/hud_gameplay.sav):
//   LEFT  (anchor to 16:9 left edge):  s_* d_* c_*  — the three stacked top-left counter clusters
//                                      (icon `_ic`, bar `_ba`, `×` mark `.X_x`, digits `_n1/_n2`)
//   CENTRE (keep centred, squeeze ok): m_*  — top-centre lives ("MARIO ×NN"); go00/01/02 health sun
//   RIGHT (anchor to 16:9 right edge): w_t0 — bottom-right FLUDD water gauge

#include "../overrides.h"
#include "../intrinsics.h"
#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <cmath>

static constexpr u32 J2DPICTURE_DRAWFULLSET = 0x802cc838u;

enum HudAnchor { A_NONE, A_LEFT, A_CENTER, A_RIGHT };

// Read a J2DPane's 4-char name fourCC (raw bytes, big-endian) into out[5].
static void read_fourcc(u32 id, char out[5]) {
    u32 t = (id >= 0x80000000u && id < 0x81800000u) ? mem_r32(id + 0x10) : 0;
    for (int i = 0; i < 4; i++) out[i] = (char)((t >> (24 - i * 8)) & 0xff);
    out[4] = 0;
}

// Classify a HUD element by its .blo name. Returns A_NONE for anything that is not one of the
// HUD's own elements (incl. every menu element) so it is passed through untouched.
static HudAnchor hud_anchor(const char n[5]) {
    // Health "sun" segments go00/go01/go02… — centre-bottom.
    if (n[0] == 'g' && n[1] == 'o' && std::isdigit((unsigned char)n[2]) && std::isdigit((unsigned char)n[3]))
        return A_CENTER;
    // Counter elements: <cluster> '_' <role>.  The `×`-mark panes (".X_x") carry a non-printable
    // lead byte, so the cluster letter is at position 0 (s_ba) or position 1 (?s_x); locate it by
    // where the underscore is — independent of the lead byte, so no '.'-substitution dependency.
    char cl, r0, r1;
    if (n[1] == '_')      { cl = n[0]; r0 = n[2]; r1 = n[3]; }   // s_ba, m_n1, w_t0
    else if (n[2] == '_') { cl = n[1]; r0 = n[3]; r1 = 0;    }   // ?s_x, ?m_x
    else                  return A_NONE;
    // HUD role gate — excludes the menu's `_0<x>` names (s_0a, .s_1, n_0a) which share the cluster
    // letters but never a HUD role.
    const bool hud_role =
        (r0 == 'b' && r1 == 'a') ||                              // _ba  background bar
        (r0 == 'i' && r1 == 'c') ||                              // _ic  icon
        (r0 == 't' && r1 == 'x') ||                              // _tx  text label (m_tx)
        (r0 == 'x') ||                                           // _x   the `×` mark (.s_x)
        ((r0 == 'n' || r0 == 't') && std::isdigit((unsigned char)r1)); // _n<d> digit, _t<d> gauge
    if (!hud_role) return A_NONE;
    switch (cl) {
        case 's': case 'd': case 'c': return A_LEFT;
        case 'm':                     return A_CENTER;
        case 'w':                     return A_RIGHT;
        default:                      return A_NONE;
    }
}

// The x-translation (640-space units) that moves a corner-anchored element from where the WS_SCALE
// squeeze leaves it (inset by the pillar) back out to the real 16:9 edge. Derived from the squeeze:
// ov_gx_projection maps guest x → screen fraction  s(x) = scale·x/640 + (1−scale)/2, so the 4:3
// frame occupies the centre `scale` of the 16:9 screen with a (1−scale)/2 pillar each side. Shifting
// a left element by −off (and a right element by +off) cancels that pillar:
//   off = ((1−scale)/2)·640/scale = 320·(1−scale)/scale   (= 106.67 px at scale 0.75).
// Returns 0 when widescreen is off (no squeeze → no anchoring needed).
static float hud_offset() {
    static const float off = [] {
        const char* w = getenv("SUNBRIGHT_WIDESCREEN");
        if (w && atoi(w) == 0) return 0.0f;
        const char* e = getenv("SUNBRIGHT_WS_SCALE");
        const float scale = e ? (float)atof(e) : 0.75f;
        if (scale <= 0.0f || scale >= 1.0f) return 0.0f;
        return 320.0f * (1.0f - scale) / scale;
    }();
    return off;
}

static void ov_drawfullset(CPUState& cpu) {
    char nm[5];
    read_fourcc(cpu.gpr[3], nm);
    const HudAnchor a = hud_anchor(nm);

    if (a == A_LEFT || a == A_RIGHT) {
        const s32 d = (s32)std::lroundf(hud_offset());
        cpu.gpr[4] = (u32)((s32)cpu.gpr[4] + (a == A_LEFT ? -d : d));
    }

    static const bool log = getenv("SUNBRIGHT_RENDERPORT_LOG") != nullptr;
    if (log) {
        char disp[5];   // printable form for the log
        for (int i = 0; i < 4; i++) disp[i] = (nm[i] >= 0x20 && nm[i] < 0x7f) ? nm[i] : '.';
        disp[4] = 0;
        const char* anc = a == A_LEFT ? "L" : a == A_RIGHT ? "R" : a == A_CENTER ? "C" : "-";
        std::fprintf(stderr, "[hud] drawFullSet '%s' anchor=%s id=%08x x->%d (y=%d %dx%d)\n",
                     disp, anc, cpu.gpr[3], (s32)cpu.gpr[4], (s32)cpu.gpr[5], (s32)cpu.gpr[6], (s32)cpu.gpr[7]);
    }

    if (RecompFunc orig = recomp_raw(J2DPICTURE_DRAWFULLSET)) orig(cpu);
    else call_ppc(cpu, cpu.lr);
}

static const bool s_hud_registered = [] {
    register_override(J2DPICTURE_DRAWFULLSET, &ov_drawfullset);
    std::fprintf(stderr, "[hud] native HUD layout: own J2DPicture::drawFullSet @ %08x\n",
                 J2DPICTURE_DRAWFULLSET);
    return true;
}();
