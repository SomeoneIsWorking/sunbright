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

// Set while inside TSMSFader::draw (the screen fader). The fade must cover the FULL screen, but the
// 2D squeeze would shrink it to the centre 75%. We use this to identify whether the fader sets its
// own projection (so we can exempt it) vs. draws into the parent 2D ortho.
static bool g_in_fade = false;

static void ov_gx_projection(CPUState& cpu) {
    const u32 mtx = cpu.gpr[3];
    const u32 type = cpu.gpr[4];
    static const bool log = getenv("SUNBRIGHT_RENDERPORT_LOG") != nullptr;
    if (log) {
        if (g_in_fade) std::fprintf(stderr, "[renderport] GXSetProjection DURING FADE type=%u m00=%.4f\n", type, mem_rf32(mtx));
        static unsigned long n = 0;
        if ((n++ % 120) == 0) std::fprintf(stderr, "[renderport] GXSetProjection#%lu type=%u m00=%.4f\n",
                                            n, type, mem_rf32(mtx));
    }

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

// ── Screen fader (TSMSFader::draw @ 0x8013fc88) ─────────────────────────────────────────────
// The fade/wipe must cover the WHOLE screen; the 2D squeeze otherwise leaves the side 12.5%
// uncovered. Wrap the draw to flag the fade window (the projection hook logs what it does so we
// can pick the fix: exempt its projection, or expand its rect).
static constexpr u32 TSMSFADER_DRAW = 0x8013fc88u;
static void ov_fader_draw(CPUState& cpu) {
    static const bool log = getenv("SUNBRIGHT_RENDERPORT_LOG") != nullptr;
    if (log) { static unsigned long n = 0;
        if ((n++ % 60) == 0) std::fprintf(stderr, "[renderport] TSMSFader::draw#%lu this=%08x rect=%08x\n",
                                          n, cpu.gpr[3], cpu.gpr[4]); }
    g_in_fade = true;
    if (RecompFunc orig = recomp_raw(TSMSFADER_DRAW)) orig(cpu);
    else call_ppc(cpu, cpu.lr);
    g_in_fade = false;
}
static const bool s_fader_registered = [] {
    if (getenv("SUNBRIGHT_RENDERPORT"))
        register_override(TSMSFADER_DRAW, &ov_fader_draw);
    return true;
}();
