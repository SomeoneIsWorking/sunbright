// Native render port — own the object model, keep Dolphin's GPU.
//
// Settled scope (docs/model_interpolation.md): hook the game's scene-graph draws / projection
// setup, drive Dolphin's GX→Vulkan backend from it. We own *what/where* (projection, 2D layout,
// transforms, interpolated in-between frames); Dolphin keeps rasterizing.
//
// ── Native 16:9 widescreen ──────────────────────────────────────────────────────────────────
// Done HERE, at the source, instead of patching .data constants or using Dolphin's ForceWide
// (which blindly stretches the whole 4:3 EFB — 2D included — so the HUD/title shift and stretch).
// We override the JDrama camera's projection-aspect setter to widen the 3D projection to 16:9.
// Dolphin's AspectMode::Auto then auto-detects each frame's real aspect from its projection: the
// widened 3D → 16:9 (presented wide), the untouched 2D ortho → 4:3 (presented centered). So 2D
// overlays keep their authored 4:3 composition, centered, while the 3D world gets the wider FOV.

#include "../overrides.h"
#include "../intrinsics.h"
#include <cstdlib>
#include <cstdio>
#include <atomic>

// GXSetProjection(const f32 mtx[4][4], GXProjectionType type) — USA/GMSE01 0x80362c34.
// The universal point EVERY 3D projection passes through (and what Dolphin's Auto aspect heuristic
// analyzes). r3 = pointer to the projection matrix, r4 = type (GX_PERSPECTIVE=0, GX_ORTHOGRAPHIC=1).
// For a perspective matrix, m[0][0] (offset 0) is the horizontal scale = cot(fovx/2); scaling it by
// 0.75 (= (4:3)/(16:9)) widens the horizontal FOV to 16:9. We touch ONLY perspective projections, so
// the 2D orthographic ones stay 4:3 → Dolphin presents 3D frames at 16:9 and 2D centered at 4:3.
static constexpr u32 GX_SET_PROJECTION = 0x80362c34u;
static constexpr u32 GX_PERSPECTIVE    = 0u;

static bool widescreen_on() {
    static const bool on = [] { const char* w = getenv("SUNBRIGHT_WIDESCREEN"); return !w || atoi(w) != 0; }();
    return on;
}

// Set while inside TGCConsole2::perform (the in-game HUD draw). The HUD is corner-anchored, so the
// centering squeeze (which keeps menus in the 4:3 safe area) would wrongly pull the gauges inward.
// Global (not static) so call_ppc can log every function called during the HUD (find element draws).
bool g_in_hud = false;

static void ov_gx_projection(CPUState& cpu) {
    const u32 mtx = cpu.gpr[3];
    const u32 type = cpu.gpr[4];
    static const bool log = getenv("SUNBRIGHT_RENDERPORT_LOG") != nullptr;
    if (log) {
        if (g_in_hud) std::fprintf(stderr, "[renderport] GXSetProjection DURING HUD type=%u m00=%.4f m03=%.4f\n",
                                   type, mem_rf32(mtx), mem_rf32(mtx + 0x0c));
        static unsigned long n = 0;
        if ((n++ % 120) == 0) std::fprintf(stderr, "[renderport] GXSetProjection#%lu type=%u m00=%.4f\n",
                                            n, type, mem_rf32(mtx)); }

    // 0.75 = (4/3)/(16/9). We horizontally squeeze BOTH projection types by this factor so the EFB
    // is rendered anamorphically and Dolphin presents it at 16:9:
    //   • perspective: scale m[0][0] (X scale) → wider horizontal FOV (true Hor+ widescreen);
    //   • orthographic (2D): scale m[0][0] AND m[0][3] (X offset) → the 2D image shrinks toward the
    //     screen centre, so after the 16:9 present it is correct-aspect and CENTERED (not stretched).
    // Both are restored after the original packs them (the game reuses the matrices).
    static const float scale = [] { const char* e = getenv("SUNBRIGHT_WS_SCALE"); return e ? (float)atof(e) : 0.75f; }();
    const bool is2d = (type != GX_PERSPECTIVE);
    // Only squeeze 2D once the game has rendered 3D (latched): the pure-2D boot/intro logos render
    // before any perspective and present at 4:3, so squeezing them would wrongly narrow them. After
    // the first 3D frame (title onward), 2D shares a 16:9 EFB and must be pre-squeezed.
    static bool seen_3d = false;
    if (!is2d) seen_3d = true;
    // The in-game HUD is corner-anchored, so its 2D ortho uses a SEPARATE squeeze factor
    // (SUNBRIGHT_HUD_SCALE, default 1.0): 1.0 = no squeeze → gauges fill to the 16:9 corners but the
    // EFB→16:9 present stretches them ~1.33×; 0.75 = full squeeze → correct aspect but centred in the
    // 4:3 safe area. Tune between for the anchor-vs-stretch tradeoff. (A true no-stretch edge anchor
    // needs per-element pre-squeeze + scissor repositioning — see docs; this knob is the interim.)
    // ALL 2D (menus AND the HUD ortho) gets the centering squeeze → correct aspect. The HUD is then
    // edge-anchored per-element by spreading each drawFullSet x (ov_drawfullset) so it lands back at
    // its authored position — un-stretched. (The old g_in_hud full-ortho exemption is gone: it
    // stretched, and g_in_hud leaks across the tail-recursive scene draw anyway.)
    bool patched = false; f32 m00 = 0.0f, m03 = 0.0f;
    if (widescreen_on() && (!is2d || seen_3d) && mtx >= 0x80000000u && mtx < 0x81800000u) {
        m00 = mem_rf32(mtx + 0x00);
        mem_wf32(mtx + 0x00, m00 * scale);
        if (is2d) { m03 = mem_rf32(mtx + 0x0c); mem_wf32(mtx + 0x0c, m03 * scale); }
        patched = true;
    }
    if (RecompFunc orig = recomp_raw(GX_SET_PROJECTION)) orig(cpu);
    else { if (patched) { mem_wf32(mtx + 0x00, m00); if (is2d) mem_wf32(mtx + 0x0c, m03); } call_ppc(cpu, cpu.lr); return; }
    if (patched) { mem_wf32(mtx + 0x00, m00); if (is2d) mem_wf32(mtx + 0x0c, m03); }   // restore
}

static const bool s_proj_registered = [] {
    register_override(GX_SET_PROJECTION, &ov_gx_projection);
    std::fprintf(stderr, "[renderport] native widescreen: hooked GXSetProjection @ %08x\n",
                 GX_SET_PROJECTION);
    return true;
}();

// ── In-game HUD (TGCConsole2::perform @ 0x8014083c) ─────────────────────────────────────────
// Found by vtable RE (perform = vtable slot 8 for JDrama::TViewObj subclasses; TGCConsole2 vtable
// @ 0x803c0304). This is the gameplay HUD draw (coins/timer/lives) — NOT J2D. Flag the window so
// its 2D ortho is edge-anchored (above) rather than centre-squeezed. Fires only in real gameplay,
// so it also confirms we reached the HUD (vs a cutscene).
static constexpr u32 TGCCONSOLE2_PERFORM = 0x8014083cu;
// Counts HUD draws — read by main_sdl to auto-save a state once the HUD is up (= real gameplay),
// so a gameplay save state can be captured by driving the game, with no manual save needed.
std::atomic<uint64_t> g_hud_perform_count{0};
static void ov_hud_perform(CPUState& cpu) {
    g_hud_perform_count.fetch_add(1, std::memory_order_relaxed);
    static const bool log = getenv("SUNBRIGHT_RENDERPORT_LOG") != nullptr;
    if (log) { static unsigned long n = 0;
        if ((n++ % 30) == 0) std::fprintf(stderr, "[renderport] TGCConsole2::perform#%lu this=%08x flags=%08x\n",
                                          n, cpu.gpr[3], cpu.gpr[4]); }
    g_in_hud = true;
    if (RecompFunc orig = recomp_raw(TGCCONSOLE2_PERFORM)) orig(cpu);
    else call_ppc(cpu, cpu.lr);
    g_in_hud = false;
}
static const bool s_hud_registered = [] {
    register_override(TGCCONSOLE2_PERFORM, &ov_hud_perform);
    return true;
}();

// Counter-element draw (shine/coins), called 6× from perform with r3 = the element sub-object;
// it reads a float position from element+0x18. Log-only first (RENDERPORT_LOG) to confirm the
// position field, toward the no-stretch fix (full-squeeze ortho + spread each element's position
// ×1.333 around centre so it lands back at its anchor at correct size).
static constexpr u32 HUD_COUNTER_DRAW = 0x8013ebf0u;
static void ov_hud_counter(CPUState& cpu) {
    static const bool log = getenv("SUNBRIGHT_RENDERPORT_LOG") != nullptr;
    const u32 e = cpu.gpr[3];
    if (log) {
        static unsigned long n = 0;
        if (n++ < 30)
            std::fprintf(stderr, "[renderport] HUD counter#%lu e=%08x +14=%.1f +18=%.1f +1c=%.1f +20=%.1f\n",
                         n, e, mem_rf32(e+0x14), mem_rf32(e+0x18), mem_rf32(e+0x1c), mem_rf32(e+0x20));
    }
    if (RecompFunc orig = recomp_raw(HUD_COUNTER_DRAW)) orig(cpu); else call_ppc(cpu, cpu.lr);
}
static const bool s_counter_registered = [] {
    if (getenv("SUNBRIGHT_RENDERPORT_LOG")) register_override(HUD_COUNTER_DRAW, &ov_hud_counter);
    return true;
}();

// perform's other element-draw calls — log which actually fire in gameplay + the object/pos, to
// pin which draws each visible element (coins/shine/water/lives/dark-overlay). Gated diagnostic.
template <u32 ADDR>
static void ov_hud_probe(CPUState& cpu) {
    static const bool log = getenv("SUNBRIGHT_RENDERPORT_LOG") != nullptr;
    if (log) { static unsigned long n = 0;
        if (n++ < 8) std::fprintf(stderr, "[renderport] HUD draw %08x fired r3=%08x r4=%08x f1=%.1f f2=%.1f\n",
                                  ADDR, cpu.gpr[3], cpu.gpr[4], cpu.fpr[1].ps0, cpu.fpr[2].ps0); }
    if (RecompFunc orig = recomp_raw(ADDR)) orig(cpu); else call_ppc(cpu, cpu.lr);
}
static const bool s_hud_probes = [] {
    if (getenv("SUNBRIGHT_RENDERPORT_LOG")) {
        register_override(0x8014ce84u, &ov_hud_probe<0x8014ce84u>);
        register_override(0x8014cc20u, &ov_hud_probe<0x8014cc20u>);
        register_override(0x8014c7e8u, &ov_hud_probe<0x8014c7e8u>);
        register_override(0x80148f64u, &ov_hud_probe<0x80148f64u>);
    }
    return true;
}();

// J2DPicture::drawFullSet(int x, int y, int w, int h, ...) @ 0x802cc838 — used by BOTH the HUD and
// menus (e.g. the file-select). The dest rect is in the ARGS (r4..r7). A blanket x-spread here was
// WRONG: it shoved the centered file-select items right. Repositioning must be PER-ELEMENT, keyed by
// the element's NAME (J2DPicture is a J2DPane → fourCC tag at this+0x10) + its draw context. This is
// log-only for now (SUNBRIGHT_RENDERPORT_LOG) to build the complete element map before touching any.
static constexpr u32 J2DPICTURE_DRAWFULLSET = 0x802cc838u;
static void elem_name(u32 id, char out[5]) {
    u32 t = (id >= 0x80000000u && id < 0x81800000u) ? mem_r32(id + 0x10) : 0;
    for (int i = 0; i < 4; i++) { u8 c = (t >> (24 - i*8)) & 0xff; out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.'; }
    out[4] = 0;
}
static void ov_drawfullset(CPUState& cpu) {
    static const bool log = getenv("SUNBRIGHT_RENDERPORT_LOG") != nullptr;
    if (log) {
        char nm[5]; elem_name(cpu.gpr[3], nm);
        std::fprintf(stderr, "[map] drawFullSet '%s' id=%08x rect=(%d,%d %dx%d) hud=%d\n",
                     nm, cpu.gpr[3], (s32)cpu.gpr[4], (s32)cpu.gpr[5], (s32)cpu.gpr[6], (s32)cpu.gpr[7], (int)g_in_hud);
    }
    if (RecompFunc orig = recomp_raw(J2DPICTURE_DRAWFULLSET)) orig(cpu); else call_ppc(cpu, cpu.lr);
}
static const bool s_drawfullset_registered = [] {
    if (getenv("SUNBRIGHT_RENDERPORT_LOG")) register_override(J2DPICTURE_DRAWFULLSET, &ov_drawfullset);
    return true;
}();

// GXSetViewport(f32 left, f32 top, f32 w, f32 h, f32 nearZ, f32 farZ) @ 0x803630c8. Log-only during
// the HUD (SUNBRIGHT_RENDERPORT_LOG) to learn how the HUD positions each element (per-element
// viewport rects in EFB pixels) — that decides the no-stretch edge-anchor.
static constexpr u32 GX_SET_VIEWPORT = 0x803630c8u;
static void ov_gx_viewport(CPUState& cpu) {
    static const bool log = getenv("SUNBRIGHT_RENDERPORT_LOG") != nullptr;
    if (log && g_in_hud) {
        static unsigned long n = 0;
        if ((n++ % 20) == 0)
            std::fprintf(stderr, "[renderport] HUD GXSetViewport left=%.1f top=%.1f w=%.1f h=%.1f\n",
                         cpu.fpr[1].ps0, cpu.fpr[2].ps0, cpu.fpr[3].ps0, cpu.fpr[4].ps0);
    }
    if (RecompFunc orig = recomp_raw(GX_SET_VIEWPORT)) orig(cpu); else call_ppc(cpu, cpu.lr);
}
static const bool s_viewport_registered = [] {
    if (getenv("SUNBRIGHT_RENDERPORT_LOG")) register_override(GX_SET_VIEWPORT, &ov_gx_viewport);
    return true;
}();

// GXSetScissor(u32 left, u32 top, u32 wd, u32 ht) @ 0x80363138 (EFB pixels). The HUD positions each
// element by its scissor rect — log them during the HUD to classify each by corner for the anchor.
static constexpr u32 GX_SET_SCISSOR = 0x80363138u;
static void ov_gx_scissor(CPUState& cpu) {
    static const bool log = getenv("SUNBRIGHT_RENDERPORT_LOG") != nullptr;
    if (log && g_in_hud)
        std::fprintf(stderr, "[renderport] HUD GXSetScissor left=%u top=%u w=%u h=%u\n",
                     cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[6]);
    if (RecompFunc orig = recomp_raw(GX_SET_SCISSOR)) orig(cpu); else call_ppc(cpu, cpu.lr);
}
static const bool s_scissor_registered = [] {
    if (getenv("SUNBRIGHT_RENDERPORT_LOG")) register_override(GX_SET_SCISSOR, &ov_gx_scissor);
    return true;
}();

// Fade-curtain probes (SUNBRIGHT_RENDERPORT_LOG). The plaza fade-in (load file A → Delfino) triggers
// the curtain. Log both candidate paths to see which draws it + its rect.
static void ov_fade_probe_window(CPUState& cpu) {
    static const bool log = getenv("SUNBRIGHT_RENDERPORT_LOG") != nullptr;
    if (log) { static unsigned long n = 0; const u32 t = cpu.gpr[3];
        if (n++ < 30 && t >= 0x80000000u && t < 0x81800000u)
            std::fprintf(stderr, "[renderport] J2DWindow this=%08x bounds{%d,%d,%d,%d} rectA=%08x\n",
                         t, (s32)mem_r32(t+0x24), (s32)mem_r32(t+0x28), (s32)mem_r32(t+0x2c), (s32)mem_r32(t+0x30), cpu.gpr[4]); }
    if (RecompFunc o = recomp_raw(0x802d18ecu)) o(cpu); else call_ppc(cpu, cpu.lr);
}
static void ov_fade_probe_fill(CPUState& cpu) {
    static const bool log = getenv("SUNBRIGHT_RENDERPORT_LOG") != nullptr;
    if (log) { static unsigned long n = 0; const u32 r = cpu.gpr[4];
        if (n++ < 12 && r >= 0x80000000u && r < 0x81800000u)
            std::fprintf(stderr, "[renderport] drawFadeinout TRect=%08x f{%.1f %.1f %.1f %.1f}\n",
                         r, mem_rf32(r+0), mem_rf32(r+4), mem_rf32(r+8), mem_rf32(r+0xc)); }
    if (RecompFunc o = recomp_raw(0x8013fa54u)) o(cpu); else call_ppc(cpu, cpu.lr);
}
// TSunGlass::draw(const JDrama::TRect&, JUtility::TColor) @ 0x8017d354 — the full-screen darkening
// curtain (plaza fade-in etc.). It positions its quad via GXLoadPosMtxImm (a modelview matrix), drawn
// in the squeezed 2D ortho → only covers the centre 75%. Flag the window so its modelview X is scaled
// by 1/0.75 (ov_gx_loadposmtx) — cancels the ortho squeeze so the solid tint covers the full 16:9.
static constexpr u32 TSUNGLASS_DRAW   = 0x8017d354u;
static constexpr u32 GX_LOADPOSMTXIMM = 0x80362e0cu;
static bool g_in_sunglass = false;
static void ov_sunglass(CPUState& cpu) {
    g_in_sunglass = true;
    if (RecompFunc o = recomp_raw(TSUNGLASS_DRAW)) o(cpu); else call_ppc(cpu, cpu.lr);
    g_in_sunglass = false;
}
// GXLoadPosMtxImm(f32 mtx[3][4], u32 id) @ 0x80362e0c — VERY hot (every 3D model). We only touch the
// matrix while drawing the sunglass curtain: scale row 0 (the X mapping) by 1/0.75 so the quad widens
// to cancel the ortho squeeze → full-screen tint. Restored after the original packs it.
static void ov_gx_loadposmtx(CPUState& cpu) {
    const u32 m = cpu.gpr[3];
    bool patched = false; f32 r0c0 = 0, r0c3 = 0;
    if (g_in_sunglass && widescreen_on() && m >= 0x80000000u && m < 0x81800000u) {
        static const float k = 1.0f / 0.75f;
        r0c0 = mem_rf32(m + 0x00); r0c3 = mem_rf32(m + 0x0c);
        mem_wf32(m + 0x00, r0c0 * k); mem_wf32(m + 0x0c, r0c3 * k);
        patched = true;
    }
    if (RecompFunc o = recomp_raw(GX_LOADPOSMTXIMM)) o(cpu); else { if (patched) { mem_wf32(m+0,r0c0); mem_wf32(m+0xc,r0c3); } call_ppc(cpu, cpu.lr); return; }
    if (patched) { mem_wf32(m + 0x00, r0c0); mem_wf32(m + 0x0c, r0c3); }
}
static const bool s_fade_probes = [] {
    register_override(TSUNGLASS_DRAW,   &ov_sunglass);        // functional (always on in widescreen)
    register_override(GX_LOADPOSMTXIMM, &ov_gx_loadposmtx);
    if (getenv("SUNBRIGHT_RENDERPORT_LOG")) {
        register_override(0x802d18ecu, &ov_fade_probe_window);
        register_override(0x8013fa54u, &ov_fade_probe_fill);
    }
    return true;
}();

// ── 2D draw scaffold (SUNBRIGHT_RENDERPORT) ─────────────────────────────────────────────────
// J2DScreen::draw(int x, int y, const J2DGrafContext*) @ 0x802cfda8 — the top-level 2D screen
// draw. Super-call wrap (recomp_raw) proven to run the original draw with no regression; r3 =
// stable J2DScreen* (object ID), r6 = J2DGrafContext (its 2D ortho is 0..640 × 0..448, stored at
// +0x08/+0x18). Kept as the foundation for future 2D ownership (backdrop expansion, capture); the
// log dumps the GrafContext for RE. Off unless SUNBRIGHT_RENDERPORT is set.
static constexpr u32 J2DSCREEN_DRAW = 0x802cfda8u;

static void ov_j2dscreen_draw(CPUState& cpu) {
    static const bool log = getenv("SUNBRIGHT_RENDERPORT_LOG") != nullptr;
    if (log) {
        static unsigned long n = 0;
        if ((n++ % 2000) == 0)
            std::fprintf(stderr, "[renderport] J2DScreen::draw screen=%08x grafctx=%08x\n",
                         cpu.gpr[3], cpu.gpr[6]);
    }
    if (RecompFunc orig = recomp_raw(J2DSCREEN_DRAW)) orig(cpu);
    else call_ppc(cpu, cpu.lr);
}

static const bool s_renderport_registered = [] {
    if (getenv("SUNBRIGHT_RENDERPORT"))
        register_override(J2DSCREEN_DRAW, &ov_j2dscreen_draw);
    return true;
}();

// NOTE: TSMSFader::draw (0x8013fc88) is NOT the fade quad — it wraps the whole 2D draw pass
// (its scope contains all the screen's projections). Flagging it and exempting "during fade"
// un-squeezed every 2D element (stretched the file-select menu), so that approach is abandoned.
// The fade-covers-full-screen fix needs a NARROWER hook on the actual solid-fill draw
// (drawFadeinout 0x8013fa54 → the GX quad), or expanding the fill rect. TODO, see docs.
