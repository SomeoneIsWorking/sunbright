// widescreen.cpp — native 16:9, done at the source rather than by stretching the picture.
//
// Ported from the retired Dolphin-era overrides (git 9283f44^: runtime/overrides/scene_render.cpp
// and cull_widescreen.cpp), which is where the RE below was established. The Dolphin era relied on
// AspectMode::Auto to notice the widened projection and present each frame at its real aspect;
// aurora owns the present here, so this file also tells aurora what aspect the picture is
// (aurora_set_present_aspect) instead of inferring it.
//
// THE APPROACH IS ANAMORPHIC. Every 3D projection passes through GXSetProjection, and we squeeze it
// horizontally by 0.75 = (4:3)/(16:9):
//
//   * perspective: scale m[0][0] -> a WIDER horizontal field of view, i.e. true Hor+ widescreen.
//     You see more of the world at the sides, rather than a cropped or stretched 4:3 image.
//   * orthographic (2D): scale m[0][0] AND m[0][3] (the X offset) -> the 2D image shrinks toward
//     the screen centre, so after the 16:9 present it is CORRECT-ASPECT and CENTRED rather than
//     stretched. Corner HUD elements are then anchored back out to the real 16:9 edges per element,
//     by name, in hud.cpp — a per-element job, because "all 2D" includes menus that must stay put.
//
// The guest reuses its projection matrices, so each one is restored immediately after the real
// GXSetProjection has packed it. Squeezing in place without restoring would compound every frame.
//
//   SBR_WIDESCREEN=0   off (4:3, the console picture)
//   SBR_WS_SCALE=<f>   override the squeeze factor (default 0.75)

#include "overrides.h"

#include <aurora/aurora.h>
#include <intrinsics.h>
#include <lucent/log.h>

#include <cmath>
#include <cstdlib>

extern "C" void func_80362c34(CPUState&);   // GXSetProjection
extern "C" void func_802260cc(CPUState&);   // SetViewFrustumClipCheckPerspective

namespace {

constexpr u32 GX_SET_PROJECTION = 0x80362c34u;
constexpr u32 SET_VIEW_FRUSTUM  = 0x802260ccu;
constexpr u32 GX_PERSPECTIVE    = 0u;

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
u32  g_ws_last_ortho = 0;

namespace {

// True once the game has drawn anything in perspective. The boot and logo screens are pure 2D and
// are presented 4:3, so squeezing them would wrongly narrow them; from the first 3D frame onward
// the 2D shares a 16:9 picture and must be pre-squeezed to survive it.
bool g_seen3d = false;

void ov_gx_set_projection(CPUState& cpu) {
    const u32 mtx  = cpu.gpr[3];
    const u32 type = cpu.gpr[4];
    const bool is2d = (type != GX_PERSPECTIVE);
    if (!is2d) g_seen3d = true;

    // Remember the live 2D ortho so a suspend scope can re-issue it unsqueezed. Orthos issued
    // INSIDE a scope are effect-internal (draw_mist's ortho lives on the guest stack), and
    // recording one would leave the reload pointer dangling once the frame returns.
    if (is2d && !g_ws_2d_suspend && sb_ram_fast(mtx) != nullptr) g_ws_last_ortho = mtx;

    const bool suspended = is2d ? g_ws_2d_suspend : g_ws_persp_suspend;
    const bool patch = widescreen_on() && !suspended && (!is2d || g_seen3d) &&
                       sb_ram_fast(mtx) != nullptr;
    f32 m00 = 0.0f, m03 = 0.0f;
    if (patch) {
        const float scale = ws_scale();
        m00 = guest_f32(mtx + 0x00);
        guest_set_f32(mtx + 0x00, m00 * scale);
        if (is2d) {
            m03 = guest_f32(mtx + 0x0C);
            guest_set_f32(mtx + 0x0C, m03 * scale);
        }
    }

    func_80362c34(cpu);   // the real GXSetProjection packs the squeezed matrix

    if (patch) {
        guest_set_f32(mtx + 0x00, m00);
        if (is2d) guest_set_f32(mtx + 0x0C, m03);
    }
}

// The game's actor culling tests against a frustum set from the CAMERA's 4:3 aspect. Widening only
// the GPU projection would leave actors inside the extra 16:9 side thirds culled, popping in and
// out at the edges. Scaling the aspect argument keeps the game's own code — including its cache,
// which is keyed on the values passed — while describing the wider frustum we actually render.
//
//   SetViewFrustumClipCheckPerspective(f32 fovy/f1, f32 aspect/f2, f32 near/f3, f32 far/f4)
void ov_set_view_frustum(CPUState& cpu) {
    if (widescreen_on()) cpu.fpr[2].ps0 *= kWideOverNarrow;
    func_802260cc(cpu);
}

// Announce the picture's aspect to aurora once, at the first projection we touch: doing it at
// static-init time would run before aurora exists.
struct AspectAnnounce {
    void operator()() {
        static bool done = false;
        if (done) return;
        done = true;
        if (widescreen_on()) {
            aurora_set_present_aspect(16, 9);
            lucent::info("widescreen", "16:9 (anamorphic, squeeze {:.4f})", ws_scale());
        } else {
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
    if (!g_ws_last_ortho) return;
    CPUState scratch = cpu;
    scratch.gpr[3] = g_ws_last_ortho;
    scratch.gpr[4] = 1;   // GX_ORTHOGRAPHIC
    ov_gx_set_projection(scratch);
}

void ws_2d_suspend_begin(CPUState& cpu) {
    g_ws_2d_suspend = true;
    ws_reload_ortho(cpu);   // unsqueezed: 0..640 now spans the whole 16:9 present
}

void ws_2d_suspend_end(CPUState& cpu) {
    g_ws_2d_suspend = false;
    ws_reload_ortho(cpu);   // re-apply the squeeze for whatever draws next
}

// The horizontal squeeze factor, for code that must reason about where geometry REALLY lands on
// the anamorphic EFB (sunmodel_widescreen.cpp's occlusion probes).
float sbr_ws_squeeze_scale() { return ws_scale(); }

// The pillar: half the width the 2D squeeze leaves empty on each side, in the game's own 640-wide
// 2D space. hud.cpp anchors corner elements back out by exactly this much. 0 when widescreen is off,
// which is what makes every HUD shift below collapse to a no-op.
int sbr_ws_pillar() {
    static int p = -1;
    if (p < 0) {
        const float s = ws_scale();
        p = (!widescreen_on() || s <= 0.0f || s >= 1.0f) ? 0 : (int)lroundf(320.0f * (1.0f - s) / s);
        if (const char* o = std::getenv("SBR_HUD_OFF")) p = std::atoi(o);
    }
    return p;
}

SB_OVERRIDE(GX_SET_PROJECTION, ov_gx_set_projection_entry, "GXSetProjection",
            "widescreen: squeeze the projection horizontally so a 16:9 field of view is rendered "
            "into the 4:3 EFB and presented wide")
SB_OVERRIDE(SET_VIEW_FRUSTUM, ov_set_view_frustum, "SetViewFrustumClipCheckPerspective",
            "widescreen: widen the CULLING frustum to match the widened projection")
