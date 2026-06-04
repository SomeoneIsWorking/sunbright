// Native HUD ownership — Super Mario Sunshine in-game HUD (TGCConsole2).
//
// We own the HUD's widescreen layout at the 2D QUAD EMITTER. Every HUD element (counter digits/
// icons/bars, lives, FLUDD water gauge, health sun) is drawn as a 2D quad in a 0..640 ortho with an
// IDENTITY position matrix, so the screen position lives entirely in the quad's vertex coordinates.
// Those coordinates are computed in J2DPicture::drawFullSet (0x802cc838) and handed to the quad
// emitter 0x802cd2ec as float args. We hook that emitter, identify the element by its J2DPane .blo
// name (this+0x10), and translate its X coordinates to anchor each cluster to the real 16:9 edge.
//
// REACHABILITY: 0x802cd2ec is called by bl from drawFullSet → emitted as call_ppc → override_lookup
// applies. This only works because drawFullSet is force-CFG'd in the recompiler (tools/recompiler/
// main.cpp kForceCFG) — otherwise linear collection truncates it and the call bounces to JIT,
// bypassing all overrides. (Full RE of why every other lever failed is in git 174984d.)
//
// WIDESCREEN MATH: ov_gx_projection (scene_render.cpp) squeezes all 2D by SUNBRIGHT_WS_SCALE (0.75)
// for correct aspect, leaving the 4:3 frame in the centre with a pillar P=320·(1−scale)/scale
// (≈107 px @0.75) each side. We push LEFT clusters out by −P and the RIGHT gauge by +P so they hug
// the 16:9 edges; centre stays. Only X moves.
//
// IDENTITY BY NAME, NEVER A FLAG. We read the element name from r3 (this) directly. The file-select
// menu also draws J2DPictures (s_0a/.s_1/n_0a/shn0/yaji/.x_0); the exact HUD role gate rejects their
// _0<x> roles → menus untouched. (Menus run with no in-game HUD, but the name gate keeps it clean.)
//
//   LEFT  s_* d_* c_*    top-left counters (shine/coin/fruit)
//   CENTER m_*  go00..02 top-centre lives + health sun
//   RIGHT  w_t0          bottom-right FLUDD water gauge

#include "../overrides.h"
#include "../intrinsics.h"
#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <cmath>

static constexpr u32 QUAD_EMITTER = 0x802cd2ecu;

bool g_2d_active = false;   // set by scene_render's GXSetProjection hook (kept for diagnostics)

enum HudAnchor { A_NONE, A_LEFT, A_CENTER, A_RIGHT };

static void read_fourcc(u32 self, char out[5]) {
    u32 t = (self >= 0x80000000u && self < 0x81800000u) ? mem_r32(self + 0x10) : 0;
    for (int i = 0; i < 4; i++) out[i] = (char)((t >> (24 - i * 8)) & 0xff);
    out[4] = 0;
}

static HudAnchor hud_anchor(const char n[5]) {
    if (n[0] == 'g' && n[1] == 'o' && std::isdigit((unsigned char)n[2]) && std::isdigit((unsigned char)n[3]))
        return A_CENTER;
    char cl, r0, r1;
    if (n[1] == '_')      { cl = n[0]; r0 = n[2]; r1 = n[3]; }
    else if (n[2] == '_') { cl = n[1]; r0 = n[3]; r1 = 0;    }
    else                  return A_NONE;
    const bool hud_role =
        (r0 == 'b' && r1 == 'a') || (r0 == 'i' && r1 == 'c') ||
        (r0 == 't' && r1 == 'x') || (r0 == 'x') ||
        ((r0 == 'n' || r0 == 't') && std::isdigit((unsigned char)r1));
    if (!hud_role) return A_NONE;
    switch (cl) {
        case 's': case 'd': case 'c': return A_LEFT;
        case 'm':                     return A_CENTER;
        case 'w':                     return A_RIGHT;
        default:                      return A_NONE;
    }
}

static int hud_pillar() {
    static const int p = [] {
        const char* w = getenv("SUNBRIGHT_WIDESCREEN");
        if (w && atoi(w) == 0) return 0;
        if (const char* o = getenv("SUNBRIGHT_HUD_OFF")) return atoi(o);
        const char* e = getenv("SUNBRIGHT_WS_SCALE");
        const float s = e ? (float)atof(e) : 0.75f;
        if (s <= 0.0f || s >= 1.0f) return 0;
        return (int)std::lround(320.0 * (1.0 - s) / s);
    }();
    return p;
}

// The 2D quad emitter (0x802cd2ec): r3=this(J2DPicture), f1..f8 = the quad's corner coords. We log
// them first to learn which args are X; the per-anchor shift is applied to the X args once confirmed.
static void ov_quad(CPUState& cpu) {
    const u32 self = cpu.gpr[3];
    char nm[5]; read_fourcc(self, nm);
    const HudAnchor a = hud_anchor(nm);

    // Shift the element's authored X by the anchor offset right before the emitter reads it. Probe
    // (SUNBRIGHT_HUD_PROBE) selects which field to shift so a diff shows which one positions the quad:
    //   1 = global rect x0 (this+0x24)   2 = local rect x0/x1 (this+0x14/0x1c)   3 = both
    // RE so far (measured by frame-diff): the EMITTER is now override-reachable (recompiler force-CFG
    // of drawFullSet). probe=1 (global) = no change — global is consumed before the emitter. probe=2
    // (local) DOES move elements, but the leaf's local rect is ALSO its clip rect, so left clusters
    // get clipped off when pushed toward the edge. The clean lever is the leaf's PARENT pane (shift it
    // once → all children + the clip move together). NEXT: walk this→parent (J2DPane parent ptr) and
    // shift the cluster-parent's rect. Default 0 = no shift (HUD renders normally) until that lands.
    static const int probe = getenv("SUNBRIGHT_HUD_PROBE") ? atoi(getenv("SUNBRIGHT_HUD_PROBE")) : 0;
    const int d = (a == A_LEFT) ? -hud_pillar() : (a == A_RIGHT ? hud_pillar() : 0);
    s32 s24 = 0, s14 = 0, s1c = 0; bool t24 = false, t1 = false;
    if (d && self >= 0x80000000u && self < 0x81800000u) {
        if (probe == 1 || probe == 3) { s24 = (s32)mem_r32(self+0x24); mem_w32(self+0x24, (u32)(s24+d)); t24 = true; }
        if (probe == 2 || probe == 3) { s14 = (s32)mem_r32(self+0x14); s1c = (s32)mem_r32(self+0x1c);
            mem_w32(self+0x14, (u32)(s14+d)); mem_w32(self+0x1c, (u32)(s1c+d)); t1 = true; }
    }

    if (RecompFunc orig = recomp_raw(QUAD_EMITTER)) orig(cpu);
    else call_ppc(cpu, cpu.lr);

    if (t24) mem_w32(self+0x24, (u32)s24);
    if (t1)  { mem_w32(self+0x14, (u32)s14); mem_w32(self+0x1c, (u32)s1c); }
}

static const bool s_hud_registered = [] {
    register_override(QUAD_EMITTER, &ov_quad);
    std::fprintf(stderr, "[hud] native HUD: own 2D quad emitter @ %08x\n", QUAD_EMITTER);
    return true;
}();
