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
#include <unordered_set>

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
        return A_CENTER;                                        // health "sun" segments
    // FLUDD nozzle indicator (bottom-right, with the water gauge): nozzle icon nz<NN>, button prompt
    // xb<NN>. (The "WATER" label w_tx and w_t1 already classify RIGHT via the cluster rules below.)
    if (n[0] == 'n' && n[1] == 'z' && std::isdigit((unsigned char)n[2]) && std::isdigit((unsigned char)n[3]))
        return A_RIGHT;
    if (n[0] == 'x' && n[1] == 'b' && std::isdigit((unsigned char)n[2]) && std::isdigit((unsigned char)n[3]))
        return A_RIGHT;
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
// Full-width band panes that must stretch to the 16:9 edges instead of anchoring: the stage-title
// card's dark masks (TConsoleStr scenario_demo_1.blo 'msk1'/'msk2' — 600x70 strips at x=0 behind
// "DELFINO PLAZA"/episode text; identified via SUNBRIGHT_2DID). Same un-squeeze math as the
// TSunGlass curtain below: x' = inv_scale·x − pillar on the this+0x84 transform.
static bool hud_stretch_band(const char n[5]) {
    return n[0] == 'm' && n[1] == 's' && n[2] == 'k' && (n[3] == '1' || n[3] == '2');
}

static void ov_quad(CPUState& cpu) {
    const u32 self = cpu.gpr[3];
    char nm[5]; read_fourcc(self, nm);
    const HudAnchor a = hud_anchor(nm);

    static constexpr u32 MTX84_M00 = 0x84;
    if (hud_stretch_band(nm) && hud_pillar() != 0 && self >= 0x80000000u && self < 0x81800000u) {
        // The squeeze scales NDC x by `scale` around the PROJECTION's own centre. TConsoleStr's
        // screen draws under the 600-wide J2D ortho (root 600x480), so the centre is 300 — not
        // 320 like TSunGlass's 640-wide quad. Un-squeeze around it: x' = (x-300)/scale + 300
        // (composed into the pane transform). With 300 it spans exactly -100..700; using the
        // 640-space pillar left the right edge ~7 units short (user-visible gap).
        const f32 inv_scale = 1.0f + (f32)hud_pillar() / 320.0f;   // = 1/scale
        const f32 kCenter = 300.0f;
        const f32 m00 = mem_rf32(self + MTX84_M00), m03 = mem_rf32(self + MTX84_M00 + 0x0c);
        mem_wf32(self + MTX84_M00, m00 * inv_scale);
        mem_wf32(self + MTX84_M00 + 0x0c, (m03 - kCenter) * inv_scale + kCenter);
        if (RecompFunc orig = recomp_raw(QUAD_EMITTER)) orig(cpu); else call_ppc(cpu, cpu.lr);
        mem_wf32(self + MTX84_M00, m00); mem_wf32(self + MTX84_M00 + 0x0c, m03);
        return;
    }

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

// The blue fill + "WATER" + nozzle are drawn by TGCConsole2::drawWater (0x801441e0) / drawJuice
// (0x80144840) via a J2DOrthoGraph — NOT the emitter — and each loads its GX position matrix via
// GXLoadPosMtxImm at one fixed call site. We shift that matrix's m03 by +pillar so everything they
// draw moves right with the (already-anchored) gauge box. Keyed on the CALL SITE (cpu.lr at the
// GXLoadPosMtxImm override) — NOT a "we're in drawWater" scope flag: drawWater calls sub-functions
// that siglongjmp out (interpreter/JIT handoff) and never return, so a flag would never be cleared
// and would shift the WHOLE screen (verified). The lr key has nothing to leak. drawWater/drawJuice
// are only override-reachable because JIT block-linking + branch-following are off (main_sdl.cpp).
static constexpr u32 GX_LOAD_POS_MTX_IMM = 0x80362e0cu;
// GXLoadPosMtxImm call sites (cpu.lr) inside the TGCConsole2 FLUDD-gauge draws. Each loads an
// identity position matrix then draws part of the gauge in the 2D ortho; shifting that matrix's m03
// by +pillar moves everything that draw emits to the 16:9 right edge with the rest of the gauge.
//   0x8014421c drawWater (water level)   0x8014487c drawJuice
//   0x8014930c func@0x801492a4 = drawWaterBack (the blue swirl circle background)
static constexpr u32 kGaugePosMtxLR[] = { 0x8014421cu, 0x8014487cu, 0x8014930cu };

// J2DPicture::draw (0x802ccef4) loads each picture's own matrix (this+0x54) at this call site. The
// FLUDD nozzle indicator (nozzle icon nz<NN>, "WATER" label w_tx, water w_t1, button prompt xb<NN>)
// and the health sun go through here — NOT the quad emitter — so we anchor them here, per element by
// name (read from this = mtx − 0x54). The matrix m03 is its on-screen X (same as go0x earlier).
static constexpr u32 J2DPIC_DRAW_POSMTX_LR = 0x802ccfc0u;

// TSunGlass::draw (0x8017d354) draws the full-screen fade/darkening curtain as ONE quad over a 4:3
// rect (0..640) with an identity matrix at this load site. Under the 2D squeeze that quad only covers
// the centre 75% → the 16:9 side strips stay un-darkened. We un-squeeze its matrix to cover the whole
// 16:9 screen: map x∈[0,640] → screen[0,1], i.e. x' = (1/scale)·x − pillar, so m00 = 1/scale and
// m03 = −pillar (the matrix is identity here). Scale-and-translate, not just the gauge's translate.
static constexpr u32 TSUNGLASS_POSMTX_LR = 0x8017d3ecu;

static void ov_water_posmtx(CPUState& cpu) {
    const u32 lr = cpu.lr, mtx = cpu.gpr[3];
    int shift = 0;
    // Whole-function gauge draws (drawWater/Juice/Back): shift by call site.
    for (u32 site : kGaugePosMtxLR) if (lr == site) { shift = hud_pillar(); break; }
    // Per-element J2DPicture::draw matrix: shift by the element's anchor (name at this+0x10 = mtx-0x44).
    if (!shift && lr == J2DPIC_DRAW_POSMTX_LR && mtx >= 0x80000054u && mtx < 0x81800000u) {
        char nm[5]; read_fourcc(mtx - 0x54, nm);
        const HudAnchor a = hud_anchor(nm);
        if (a == A_LEFT)  shift = -hud_pillar();
        if (a == A_RIGHT) shift =  hud_pillar();
    }

    // Fade curtain: un-squeeze to full screen (scale m00 + translate m03), restore after.
    if (lr == TSUNGLASS_POSMTX_LR && hud_pillar() != 0 && mtx >= 0x80000000u && mtx < 0x81800000u) {
        const f32 inv_scale = 1.0f + (f32)hud_pillar() / 320.0f;          // = 1/scale
        const f32 m00 = mem_rf32(mtx + 0x00), m03c = mem_rf32(mtx + 0x0c);
        mem_wf32(mtx + 0x00, m00 * inv_scale);
        mem_wf32(mtx + 0x0c, m03c - (f32)hud_pillar());
        if (RecompFunc orig = recomp_raw(GX_LOAD_POS_MTX_IMM)) orig(cpu); else call_ppc(cpu, cpu.lr);
        mem_wf32(mtx + 0x00, m00); mem_wf32(mtx + 0x0c, m03c);
        return;
    }

    f32 m03 = 0.0f; bool moved = false;
    if (shift && mtx >= 0x80000000u && mtx < 0x81800000u) {
        m03 = mem_rf32(mtx + 0x0c);
        mem_wf32(mtx + 0x0c, m03 + (f32)shift);
        moved = true;
    }
    if (RecompFunc orig = recomp_raw(GX_LOAD_POS_MTX_IMM)) orig(cpu);
    else call_ppc(cpu, cpu.lr);
    if (moved) mem_wf32(mtx + 0x0c, m03);
}

static const bool s_hud_registered = [] {
    register_override(QUAD_EMITTER, &ov_quad);
    register_override(GX_LOAD_POS_MTX_IMM, &ov_water_posmtx);
    std::fprintf(stderr, "[hud] native HUD: own 2D quad emitter @ %08x + water gauge\n", QUAD_EMITTER);
    return true;
}();
