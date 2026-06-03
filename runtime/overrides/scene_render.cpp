// Native render port — own the object model, keep Dolphin's GPU.
//
// Settled scope (docs/model_interpolation.md): hook the game's scene-graph draws, build our own
// per-object model, and drive Dolphin's GX→Vulkan backend from it. We own *what/where*
// (transforms, 2D layout, interpolated in-between frames); Dolphin keeps rasterizing. This one
// interception layer serves BOTH open render problems — widescreen 2D layout (center overlays /
// expand backdrops) and N64Recomp-style transform interpolation.
//
// This file is the foundation increment: a *super-call* hook on J2DScreen::draw. The override
// runs the ORIGINAL draw (via recomp_raw — bypassing itself), so with no adjustment yet the frame
// is byte-identical — that's the point of this step: prove we can wrap a draw without regressing
// it, and surface the J2DGrafContext layout (the 2D ortho = the layout control point) so the next
// increment can center/expand 2D for 16:9. Env-gated: zero effect unless SUNBRIGHT_RENDERPORT=1.

#include "../overrides.h"
#include "../intrinsics.h"
#include <cstdlib>
#include <cstdio>

// J2DScreen::draw(int x, int y, const J2DGrafContext*) — USA/GMSE01 0x802cfda8.
// Verified live (SUNBRIGHT_WATCH=802cfda8): fires on the title screen; r3 = J2DScreen* (stable
// object ID), r4 = x, r5 = y, r6 = J2DGrafContext*. The GrafContext holds the 2D ortho bounds —
// the thing we adjust to fix the off-center logo / un-expanded backdrops under 16:9.
static constexpr u32 J2DSCREEN_DRAW = 0x802cfda8u;

static void ov_j2dscreen_draw(CPUState& cpu) {
    // RE aid: dump the screen pointer + a window of the GrafContext as floats so we can locate the
    // ortho (left/right/top/bottom) for the layout fix. Throttled — this is called thousands/frame.
    static const bool log = getenv("SUNBRIGHT_RENDERPORT_LOG") != nullptr;
    if (log) {
        static unsigned long n = 0;
        if ((n++ % 2000) == 0) {
            const u32 screen = cpu.gpr[3], grafctx = cpu.gpr[6];
            std::fprintf(stderr, "[renderport] J2DScreen::draw screen=%08x x=%d y=%d grafctx=%08x\n",
                         screen, (s32)cpu.gpr[4], (s32)cpu.gpr[5], grafctx);
            if (grafctx >= 0x80000000u && grafctx < 0x81800000u) {
                std::fprintf(stderr, "  grafctx floats:");
                for (u32 off = 0; off <= 0x40; off += 4)
                    std::fprintf(stderr, " +%02x=%.2f", off, mem_rf32(grafctx + off));
                std::fprintf(stderr, "\n");
            }
        }
    }

    // Super-call the real draw. Its blr is a C return in the single-call model, so control comes
    // back here and we simply return — exactly as if the override weren't present. No layout change
    // yet: this increment only establishes the wrap (verify: the title logo still renders).
    if (RecompFunc orig = recomp_raw(J2DSCREEN_DRAW)) orig(cpu);
    else call_ppc(cpu, cpu.lr);   // not recompiled (shouldn't happen — it is) → degrade gracefully
}

static const bool s_renderport_registered = [] {
    if (getenv("SUNBRIGHT_RENDERPORT")) {
        register_override(J2DSCREEN_DRAW, &ov_j2dscreen_draw);
        std::fprintf(stderr, "[renderport] hooked J2DScreen::draw @ %08x (super-call wrap)\n",
                     J2DSCREEN_DRAW);
    }
    return true;
}();
