// widescreen.cpp — native 16:9, done at the source rather than by stretching the picture.
//
// Ported from the retired Dolphin-era overrides (git 9283f44^: runtime/overrides/scene_render.cpp
// and cull_widescreen.cpp), which is where the RE below was established. The Dolphin era relied on
// AspectMode::Auto to notice the widened projection and present each frame at its real aspect;
// aurora owns the present here, so this file also tells aurora what aspect the picture is
// (aurora_set_present_aspect) instead of inferring it.
//
// THE APPROACH IS ANAMORPHIC, and — crucially — it widens 3D at the INPUT, not the output. 3D is
// widened where the aspect enters the math (C_MTXPerspective), NOT by squeezing the finished
// projection at GXSetProjection. Every perspective the game builds — the main camera AND the
// projected-texgen "effect" matrix that screen effects (heat haze, water refraction, DebuTelesa,
// MapObj/NPC distortions) rebuild from the same camera aspect — passes through C_MTXPerspective, so
// widening the aspect there makes ALL of them come out 16:9 CONSISTENTLY. That is the difference
// between porting widescreen and patching it: squeezing the projection output left every one of
// those effects reading an un-widened aspect and ghosting a second copy of the scene, which then
// needed a separate patch per effect. Widen the one shared input and there is nothing to patch.
//
//   * perspective: widened at C_MTXPerspective (aspect *= (16:9)/(4:3)) -> a wider horizontal field
//     of view, true Hor+. The result is rendered into the 4:3 EFB (anamorphic) and presented wide.
//   * orthographic (2D): built by C_MTXOrtho with no aspect, so it can't be widened that way; it is
//     squeezed here at GXSetProjection (m[0][0] and m[0][3]) so the 2D image shrinks toward centre
//     and, after the 16:9 present, is correct-aspect and CENTRED. Corner HUD elements are anchored
//     back out to the real edges per element in hud.cpp.
//
//   SBR_WIDESCREEN=0   off (4:3, the console picture)
//   SBR_WS_SCALE=<f>   override the factor (default 0.75 = the 2D squeeze; 3D widens by its
//   inverse)

#include "overrides.h"

#include "../runtime/render/native_render.h"
#include "../runtime/render/scene.h"

#include <aurora/aurora.h>
#include <intrinsics.h>
#include <lucent/log.h>

#include <cmath>
#include <cstdlib>

extern "C" void func_80362c34(CPUState&); // GXSetProjection
extern "C" void func_802260cc(CPUState&); // SetViewFrustumClipCheckPerspective
extern "C" void func_8034a404(CPUState&); // C_MTXPerspective

namespace {

constexpr u32 GX_SET_PROJECTION = 0x80362c34u;
constexpr u32 SET_VIEW_FRUSTUM = 0x802260ccu;
constexpr u32 GX_PERSPECTIVE = 0u;

// 16:9 over 4:3 — the one number the whole feature is derived from.
constexpr float kWideOverNarrow = (16.0f / 9.0f) / (4.0f / 3.0f);

bool widescreen_on() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SBR_WIDESCREEN");
        v = (e != nullptr && e[0] == '0') ? 0 : 1;
    }
    return v == 1;
}

float ws_scale() {
    static float s = 0.0f;
    if (s == 0.0f) {
        const char* e = std::getenv("SBR_WS_SCALE");
        s = (e != nullptr && e[0] != '\0') ? (float)std::atof(e) : 1.0f / kWideOverNarrow;
    }
    return s;
}

f32 guest_f32(u32 ea) {
    const u32 bits = sb_r32(ea);
    f32 f;
    __builtin_memcpy(&f, &bits, sizeof f);
    return f;
}

void guest_set_f32(u32 ea, f32 v) {
    u32 bits;
    __builtin_memcpy(&bits, &v, sizeof bits);
    sb_w32(ea, bits);
}

} // namespace

// ── Suspend scopes ───────────────────────────────────────────────────────────────────────
// Some draws must NOT be squeezed, and they fall into two kinds:
//
//   * full-screen 2D that is authored 0..640 and must span the whole 16:9 picture (fades,
//     wipes, the dash-blur quad). For these the last 2D ortho is re-issued UNSQUEEZED for the
//     duration, so 0..640 covers the real screen, then re-squeezed after.
//   * offscreen passes whose output is consumed through a matrix built from the UNsqueezed
//     camera (the EFB->texture passes, the mirror pre-render). Squeezing the render but not
//     the lookup shifts the result ~25% toward the texture centre.
//
// Flags rather than a stack because the guest reaches these through perform() dispatch; each
// scope saves and restores the previous value.
bool g_ws_2d_suspend = false;
bool g_ws_persp_suspend = false;
u32 g_ws_last_ortho = 0;
// Which projection kind the game last set. A 2D effect wrapper that re-issues an ortho while a
// PERSPECTIVE is current leaves the wrong projection behind for whatever draws next, so a wrapper
// needs to be able to ask.
bool g_ws_last_proj_is2d = false;

namespace {

// True once the game has drawn anything in perspective. The boot and logo screens are pure 2D and
// are presented 4:3, so squeezing them would wrongly narrow them; from the first 3D frame onward
// the 2D shares a 16:9 picture and must be pre-squeezed to survive it.
bool g_seen3d = false;

void ov_gx_set_projection(CPUState& cpu) {
    const u32 mtx = cpu.gpr[3];
    const u32 type = cpu.gpr[4];
    const bool is2d = (type != GX_PERSPECTIVE);
    if (!is2d)
        g_seen3d = true;
    g_ws_last_proj_is2d = is2d;

    // Remember the live 2D ortho so a suspend scope can re-issue it unsqueezed. Orthos issued
    // INSIDE a scope are effect-internal (draw_mist's ortho lives on the guest stack), and
    // recording one would leave the reload pointer dangling once the frame returns.
    if (is2d && !g_ws_2d_suspend && sb_ram_fast(mtx) != nullptr)
        g_ws_last_ortho = mtx;

    // Hand the perspective to the GX compatibility renderer. This is the one place the game's own
    // projection is available as a matrix, and it is already WIDENED (the widening happens at the
    // C_MTXPerspective input), so the native path inherits 16:9 for free rather than rebuilding it.
    if (sb_ram_fast(mtx) != nullptr) {
        float p[16];
        for (int i = 0; i < 16; ++i)
            p[i] = guest_f32(mtx + (u32)i * 4);
        // EVERY projection, 2D included. The compatibility renderer draws whatever J3DShape::draw
        // hands it, and that includes 2D elements (HUD, message box) which the game draws under an
        // ORTHO projection. Feeding those through the 3D perspective blew them up to cover the
        // frame — opaque, depth-test disabled, drawn last, hiding the entire plaza behind them.
        sbr_gx_set_projection(p, is2d);
        if (!is2d)
            sbr_scene_set_projection(p);
    }

    // 3D (perspective) is NOT squeezed here anymore. It is widened at its SOURCE — the aspect
    // passed to C_MTXPerspective (see ov_c_mtx_perspective) — so the main projection AND every
    // screen effect that rebuilds a perspective from the same camera aspect come out 16:9
    // consistently. Squeezing the projection OUTPUT here left those effects reading an unsqueezed
    // aspect and ghosting; widening the INPUT is the actual port, not a per-effect patch.
    //
    // 2D (ortho) does NOT go through C_MTXPerspective — it is built by C_MTXOrtho with no aspect —
    // so it is still squeezed here to keep menus/HUD correct-aspect and centred in the 16:9 frame.
    const bool patch =
        is2d && widescreen_on() && !g_ws_2d_suspend && g_seen3d && sb_ram_fast(mtx) != nullptr;
    f32 m00 = 0.0f, m03 = 0.0f;
    if (patch) {
        const float scale = ws_scale();
        m00 = guest_f32(mtx + 0x00);
        m03 = guest_f32(mtx + 0x0C);
        guest_set_f32(mtx + 0x00, m00 * scale);
        guest_set_f32(mtx + 0x0C, m03 * scale);
    }

    func_80362c34(cpu); // the real GXSetProjection packs the (2D-squeezed) matrix

    if (patch) {
        guest_set_f32(mtx + 0x00, m00);
        guest_set_f32(mtx + 0x0C, m03);
    }
}

// C_MTXPerspective(Mtx44 m/r3, f32 fovy/f1, f32 aspect/f2, f32 near/f3, f32 far/f4) — the ONE math
// seam every perspective in the game is built through: the main camera's projection AND the
// projected-texgen "effect" matrix that screen effects (heat haze, water refraction, DebuTelesa,
// the MapObj/NPC distortions) rebuild from the same camera fovy/aspect. Widening the aspect here —
// once, at the source — makes all of them render 16:9 CONSISTENTLY, which is what stops the screen
// effects from ghosting. There is nothing to patch per-effect because they all read this.
//
// Measured: only one aspect (~1.346, the 4:3 render aspect) ever flows through here, so widening
// unconditionally cannot corrupt some other-aspect projection. g_ws_persp_suspend still exempts the
// offscreen passes (mirror pre-render) that must stay at the un-widened aspect to match their own
// lookup matrices.
void ov_c_mtx_perspective(CPUState& cpu) {
    if (widescreen_on() && !g_ws_persp_suspend)
        cpu.fpr[2].ps0 *= (double)kWideOverNarrow; // 4:3 aspect -> 16:9
    func_8034a404(cpu);
}

// The game's actor culling tests against a frustum set from the CAMERA's 4:3 aspect. Widening only
// the GPU projection would leave actors inside the extra 16:9 side thirds culled, popping in and
// out at the edges. Scaling the aspect argument keeps the game's own code — including its cache,
// which is keyed on the values passed — while describing the wider frustum we actually render.
//
//   SetViewFrustumClipCheckPerspective(f32 fovy/f1, f32 aspect/f2, f32 near/f3, f32 far/f4)
void ov_set_view_frustum(CPUState& cpu) {
    if (widescreen_on())
        cpu.fpr[2].ps0 *= kWideOverNarrow;
    func_802260cc(cpu);
}

// Announce the picture's aspect to aurora once, at the first projection we touch: doing it at
// static-init time would run before aurora exists.
struct AspectAnnounce {
    void operator()() {
        static bool done = false;
        if (done)
            return;
        done = true;
        if (widescreen_on()) {
            aurora_set_present_aspect(16, 9);
            sbr_render_set_present_aspect(16, 9);
            lucent::info("widescreen", "16:9 (anamorphic, squeeze {:.4f})", ws_scale());
        } else {
            sbr_render_set_present_aspect(4, 3);
            lucent::info("widescreen", "off — 4:3");
        }
    }
};

void ov_gx_set_projection_entry(CPUState& cpu) {
    AspectAnnounce{}();
    ov_gx_set_projection(cpu);
}

} // namespace

// Re-issue the last 2D ortho through the real GXSetProjection, with the squeeze suspended or
// restored according to the current flag. A scratch CPUState is enough: GXSetProjection preserves
// the callee-saved registers the caller cares about.
static void ws_reload_ortho(CPUState& cpu) {
    if (!g_ws_last_ortho)
        return;
    CPUState scratch = cpu;
    scratch.gpr[3] = g_ws_last_ortho;
    scratch.gpr[4] = 1; // GX_ORTHOGRAPHIC
    ov_gx_set_projection(scratch);
}

void ws_2d_suspend_begin(CPUState& cpu) {
    g_ws_2d_suspend = true;
    ws_reload_ortho(cpu); // unsqueezed: 0..640 now spans the whole 16:9 present
}

void ws_2d_suspend_end(CPUState& cpu) {
    g_ws_2d_suspend = false;
    ws_reload_ortho(cpu); // re-apply the squeeze for whatever draws next
}

// The pillar: half the width the 2D squeeze leaves empty on each side, in the game's own 640-wide
// 2D space. hud.cpp anchors corner elements back out by exactly this much. 0 when widescreen is
// off, which is what makes every HUD shift below collapse to a no-op.
int sbr_ws_pillar() {
    static int p = -1;
    if (p < 0) {
        const float s = ws_scale();
        p = (!widescreen_on() || s <= 0.0f || s >= 1.0f) ? 0
                                                         : (int)lroundf(320.0f * (1.0f - s) / s);
        if (const char* o = std::getenv("SBR_HUD_OFF"))
            p = std::atoi(o);
    }
    return p;
}

float sbr_ws_scale() {
    return widescreen_on() ? ws_scale() : 1.0f;
}

SB_OVERRIDE(0x8034a404u, ov_c_mtx_perspective, "C_MTXPerspective",
            "widescreen: widen the aspect at its source so the main projection and every "
            "screen-projected effect render 16:9 consistently — no per-effect patching")
SB_OVERRIDE(GX_SET_PROJECTION, ov_gx_set_projection_entry, "GXSetProjection",
            "widescreen: squeeze only the 2D orthographic projection (built by C_MTXOrtho, not "
            "C_MTXPerspective) so menus and HUD stay correct-aspect and centred")
SB_OVERRIDE(SET_VIEW_FRUSTUM, ov_set_view_frustum, "SetViewFrustumClipCheckPerspective",
            "widescreen: widen the CULLING frustum to match — it takes the camera aspect directly, "
            "not via C_MTXPerspective")
