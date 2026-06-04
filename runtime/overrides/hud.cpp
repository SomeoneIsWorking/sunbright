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

    // The element's screen position is the X translation (m03) of its transform matrix at this+0x84
    // (verified: m03 == the element's 640-space screen X — counters 13, lives 223, water 515). The
    // emitter builds the GX position matrix from this+0x84 right here, so shifting m03 by the pillar
    // offset moves the element to its 16:9 edge — and it's a TRANSFORM, not a clip rect, so nothing
    // is clipped away. We restore it after the draw (the matrix is persistent object state).
    static constexpr u32 MTX84_M03 = 0x90;   // this+0x84 + 0x0c
    f32 m03 = 0.0f; bool moved = false;
    if ((a == A_LEFT || a == A_RIGHT) && self >= 0x80000000u && self < 0x81800000u) {
        m03 = mem_rf32(self + MTX84_M03);
        mem_wf32(self + MTX84_M03, m03 + (f32)(a == A_LEFT ? -hud_pillar() : hud_pillar()));
        moved = true;
    }

    if (RecompFunc orig = recomp_raw(QUAD_EMITTER)) orig(cpu);
    else call_ppc(cpu, cpu.lr);

    if (moved) mem_wf32(self + MTX84_M03, m03);   // restore persistent matrix
}

// ── FLUDD water gauge blue fill — TODO ──────────────────────────────────────────────────────────
// The gauge's outline box (w_t0) goes through the emitter above and IS anchored. The blue liquid +
// "WATER" + nozzle are drawn by TGCConsole2::drawWater (0x801441e0) / drawJuice (0x80144840) via a
// J2DOrthoGraph, called DIRECTLY from perform — which runs JIT-block-linked, so overrides on those
// functions never fire (verified). Reaching them needs the same recompiler force-CFG treatment as
// drawFullSet but for perform (it's fragmented in the function list, so not a one-liner). Until then
// the blue fill stays at the un-anchored 4:3 position next to the (anchored) box.

// The blue fill + "WATER" + nozzle are drawn by drawWater/drawJuice via a J2DOrthoGraph; both load a
// GX position matrix at the start of their draw. We scope a +pillar shift of that matrix's m03 to
// those functions so everything they draw moves right with the (already-anchored) gauge box. These
// are called directly from TGCConsole2::perform; they're only override-reachable because JIT
// block-linking + branch-following are disabled by default (main_sdl.cpp) so every block dispatches
// through our hook (otherwise perform links/inlines them and the overrides never fire).
static constexpr u32 GX_LOAD_POS_MTX_IMM = 0x80362e0cu;
static int g_water_pillar = 0;

template <u32 ADDR>
static void ov_water_draw(CPUState& cpu) {
    const int prev = g_water_pillar;
    g_water_pillar = hud_pillar();
    if (RecompFunc orig = recomp_raw(ADDR)) orig(cpu);
    else call_ppc(cpu, cpu.lr);
    g_water_pillar = prev;
}

static void ov_water_posmtx(CPUState& cpu) {
    const u32 mtx = cpu.gpr[3];
    f32 m03 = 0.0f; bool moved = false;
    if (g_water_pillar && mtx >= 0x80000000u && mtx < 0x81800000u) {
        m03 = mem_rf32(mtx + 0x0c);
        mem_wf32(mtx + 0x0c, m03 + (f32)g_water_pillar);
        moved = true;
    }
    if (RecompFunc orig = recomp_raw(GX_LOAD_POS_MTX_IMM)) orig(cpu);
    else call_ppc(cpu, cpu.lr);
    if (moved) mem_wf32(mtx + 0x0c, m03);
}

static const bool s_hud_registered = [] {
    register_override(QUAD_EMITTER, &ov_quad);
    register_override(0x801441e0u, &ov_water_draw<0x801441e0u>);   // TGCConsole2::drawWater
    register_override(0x80144840u, &ov_water_draw<0x80144840u>);   // TGCConsole2::drawJuice
    register_override(GX_LOAD_POS_MTX_IMM, &ov_water_posmtx);
    std::fprintf(stderr, "[hud] native HUD: own 2D quad emitter @ %08x + water gauge\n", QUAD_EMITTER);
    return true;
}();
