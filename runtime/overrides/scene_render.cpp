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

// The anamorphic squeeze factor (4:3)/(16:9) = 0.75 — shared with every consumer that must
// reason about where squeezed geometry actually lands in the EFB (sun occlusion probes etc.).
float ws_squeeze_scale() {
    static const float scale = [] { const char* e = getenv("SUNBRIGHT_WS_SCALE"); return e ? (float)atof(e) : 0.75f; }();
    return scale;
}

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
    // The projection is published to the native renderer (ngx_set_projection) BELOW, after
    // the widescreen squeeze is applied to the guest matrix — so ngx gets the EXACT matrix
    // GX packs (perspective AND orthographic, both squeezed identically). Publishing the
    // pre-squeeze copy here (and dropping ortho) was the title/file-select bug: J3D shapes
    // drawn under an orthographic projection (the title logo) were projected with a stale
    // perspective matrix → foreshortened/sheared.
    extern void ngx_set_projection(const float*, unsigned);
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
    const float scale = ws_squeeze_scale();
    const bool is2d = (type != GX_PERSPECTIVE);
    // Tell the HUD module which projection mode is current: position matrices loaded while a 2D
    // orthographic projection is active are 2D elements (the in-game HUD, once 3D has been seen).
    extern bool g_2d_active; g_2d_active = is2d;
    // Only squeeze 2D once the game has rendered 3D (latched): the pure-2D boot/intro logos render
    // before any perspective and present at 4:3, so squeezing them would wrongly narrow them. After
    // the first 3D frame (title onward), 2D shares a 16:9 EFB and must be pre-squeezed.
    static bool seen_3d = false;
    if (!is2d) seen_3d = true;
    // ALL 2D (menus AND the in-game HUD) gets this centring squeeze → correct aspect, content in the
    // centre `scale` of the 16:9 screen. The HUD's corner gauges are then edge-anchored back out to
    // the real 16:9 edges PER ELEMENT in hud.cpp (it owns drawFullSet and shifts each element's x by
    // the pillar width). Doing it per-element by name — not via a "during HUD" ortho exemption —
    // avoids the tail-recursive flag leak that previously stretched/shifted the menus.
    // Fader/wipe scope (fader_widescreen.cpp): full-screen curtains must span the whole 16:9
    // present, so the squeeze is suspended and the ortho reloaded for the duration of
    // TSMSFader::draw. Record the last 2D ortho so the fader wrap can re-issue it.
    extern bool g_ws_2d_suspend; extern bool g_ws_persp_suspend; extern u32 g_ws_last_ortho;
    // Don't record orthos issued inside a suspend scope: those are effect-internal
    // (e.g. draw_mist's EFB-pixel ortho lives on the guest STACK — recording it would
    // leave the fader's reload pointer dangling once the frame returns).
    if (is2d && mtx >= 0x80000000u && !g_ws_2d_suspend) g_ws_last_ortho = mtx;
    bool patched = false; f32 m00 = 0.0f, m03 = 0.0f;
    if (widescreen_on() && !(is2d && g_ws_2d_suspend) && !(!is2d && g_ws_persp_suspend) &&
        (!is2d || seen_3d) && mtx >= 0x80000000u && mtx < 0x81800000u) {
        m00 = mem_rf32(mtx + 0x00);
        mem_wf32(mtx + 0x00, m00 * scale);
        if (is2d) { m03 = mem_rf32(mtx + 0x0c); mem_wf32(mtx + 0x0c, m03 * scale); }
        patched = true;
    }
    // Publish the (now-squeezed) matrix GX will pack to the native renderer, with its type.
    // ngx applies whichever projection is current per shape — so ortho-drawn J3D (the title
    // logo) gets its ortho matrix, not a stale perspective one.
    if (mtx >= 0x80000000u && mtx < 0x81800000u) {
        float pm[16];
        for (int i = 0; i < 16; i++) pm[i] = mem_rf32(mtx + i * 4);
        ngx_set_projection(pm, type);
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

// The in-game HUD's per-element widescreen layout (edge-anchoring the corner gauges) is owned
// natively in runtime/overrides/hud.cpp — it takes over J2DPicture::drawFullSet (0x802cc838) and
// repositions each HUD element by its .blo name. (The RE scaffolding that lived here — the
// 0x8013ebf0 counter probe, the perform bl-target probes, the GXSetViewport/GXSetScissor HUD logs —
// is removed now that the element map is pinned: viewport is full-screen and scissor is just the
// subtitle strip, neither positions the gauges; the gauges are positioned by drawFullSet's args.)

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
// NOTE: the TSunGlass curtain fix via an always-registered GXLoadPosMtxImm hook gated on a
// g_in_sunglass flag was REVERTED — that flag leaks across the tail-recursive scene draw (same leak
// g_in_hud has), so the modelview-X scale bled onto non-curtain draws and right-shifted the
// file-select. A correct curtain fix must NOT use a leaky draw-window flag on a hot global hook.
static const bool s_fade_probes = [] {
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

void sb_j2d_set_root(u32 root);   // runtime/render/j2d_walk.cpp — publishes the live root for /j2d
void sb_j2d_capture(u32 root);    // runtime/render/j2d_walk.cpp — snapshots the post-draw pane tree
void ngx_frame_publish();         // runtime/overrides/ngx_j3d_shape.cpp — publish the 3D frame snapshot

static void ov_j2dscreen_draw(CPUState& cpu) {
    const u32 root = cpu.gpr[3];
    // J2DScreen::draw runs once per frame AFTER all 3D drawing (the HUD draws on top),
    // so the 3D capture buffer holds a complete frame here. Publish it as the explicit
    // per-frame boundary for the native present (so it reads a whole frame, not a
    // half-accumulated one) — aligned with the J2D HUD snapshot taken just below.
    ngx_frame_publish();
    sb_j2d_set_root(root);         // publish the live root J2DScreen* (the /j2d diagnostic probes read it)
    static const bool log = getenv("SUNBRIGHT_RENDERPORT_LOG") != nullptr;
    if (log) {
        static unsigned long n = 0;
        if ((n++ % 2000) == 0)
            std::fprintf(stderr, "[renderport] J2DScreen::draw screen=%08x grafctx=%08x\n",
                         cpu.gpr[3], cpu.gpr[6]);
    }
    if (RecompFunc orig = recomp_raw(J2DSCREEN_DRAW)) orig(cpu);
    else call_ppc(cpu, cpu.lr);
    // Capture AFTER the draw: mGlobalBounds/mColorAlpha are now consistently computed
    // for every visible pane. The native present (video thread) reads this snapshot —
    // a live cross-thread walk would catch the tree mid-update (the HUD-smear bug).
    sb_j2d_capture(root);
}

// Registered unconditionally now: the tee is behavior-neutral (it super-calls the
// original draw) and is the canonical capture of the live 2D root for the native
// J2D renderer. The renderport logging inside stays env-gated.
static const bool s_renderport_registered = [] {
    register_override(J2DSCREEN_DRAW, &ov_j2dscreen_draw);
    return true;
}();

// NOTE: TSMSFader::draw (0x8013fc88) is NOT the fade quad — it wraps the whole 2D draw pass
// (its scope contains all the screen's projections). Flagging it and exempting "during fade"
// un-squeezed every 2D element (stretched the file-select menu), so that approach is abandoned.
// The fade-covers-full-screen fix needs a NARROWER hook on the actual solid-fill draw
// (drawFadeinout 0x8013fa54 → the GX quad), or expanding the fill rect. TODO, see docs.
