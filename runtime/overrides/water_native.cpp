// Water rendering — native port of the screen-texture refraction lookup math (GMSE01).
// Full RE: docs/decomp/water_rendering.md. Supersedes the water_widescreen.cpp draft.
//
// What is owned natively: the two per-frame lookup-matrix derivations that map the scene
// behind a water surface into screen-texture UVs —
//
//  1. SMS_GetLightPerspectiveForEffectMtx 0x8022ba74 (MarioUtil/MtxUtil): builds the J3D
//     effect matrix (texmtx 1, material 0) for EVERY screen-texture refraction material —
//     the sea ("SeaIndirect"/"sea"/pollution seas via TMapStaticObj::perform 0x80196614,
//     bl verified at 0x8019674c), telesa, namekuri, TIceBlock, TBEelTears, NpcParts glow,
//     MapObjBlock. Disasm-verified shape: C_MTXPerspective(out, cam->fovy[+0x48],
//     cam->aspect[+0x4C], near, far) then rows 2/3 overwritten with constants — so
//     out[0][0] = cot(fovy/2)/aspect is the ONLY aspect-dependent element.
//
//  2. TModelWaterManager::drawRefracAndSpec 0x8027c12c pass 1 TEXMTX0:
//     C_MTXLightPerspective(m, fovy, aspect, 0.5, -0.5, 0.5, 0.5) at 0x8034a17c —
//     m[0][0] = 0.5*cot(fovy/2)/aspect is again the only aspect-dependent element.
//     The fix is SCOPED: other C_MTXLightPerspective callers (mirror pipeline — its
//     texture is deliberately rendered unsqueezed; JPA projected particles) must not be
//     touched.
//
// Both derivations use the camera's STORED aspect (gpCamera+0x4C, always 4/3), not the
// live GX projection. Aspect parameterization: m00 ∝ 1/aspect, so rendering with an
// effective aspect A is EXACTLY m00 ×= (4/3)/A — the same factor ws_squeeze_scale()
// applies to the raster projection. The override therefore super-calls the guest body
// (recomp_raw — keeps the guest trig bit-exact and the body A/B-able) and applies the
// natively computed scale to m00. Pure-math reference derivations below are standalone
// and testable.
//
// Modes:
//   default — lookup m00 scaled by the LIVE raster squeeze (ws_squeeze_scale): matches the
//             squeezed projection at 16:9, exactly 1.0 (no memory touch) at 4:3. The old
//             SUNBRIGHT_WATER_WS opt-in gate is gone — user-verified in Delfino 2026-06-12
//             (the pier/sea silhouette smear), and 4:3 is identical by construction.
//   SUNBRIGHT_WATER_NATIVE=0 — disable both overrides (pure guest path, A/B).
//
// FOR THE MAIN SESSION — verification plan (docs/decomp/water_rendering.md §Verification):
//   1. 4:3 gate: SUNBRIGHT_WIDESCREEN=0, WATER_WS unset → frame dumps pixel-identical to a
//      pre-water_native build (Delfino sea visible from spawn).
//   2. SUNBRIGHT_WATER_WS=1 + widescreen: Delfino sea from spawn (effect-mtx seam) —
//      refraction aligns with the scene at the frame edges.
//   3. FLUDD spray toward camera / dive (drawRefracAndSpec seam) — droplet refraction
//      aligned at edges.
//   4. Hose splash / fountains (TSplashManager, view-space) — unchanged in all modes.
//   5. Mirror regression: bathroom mirror unchanged (scope must not leak to the mirror's
//      C_MTXLightPerspective).

#include "../overrides.h"
#include "../intrinsics.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

float ws_squeeze_scale();   // scene_render.cpp (4:3→present squeeze, e.g. 0.75 at 16:9)

namespace {

constexpr u32 kGetLightPerspForEffectMtx = 0x8022ba74u;  // SMS_GetLightPerspectiveForEffectMtx
constexpr u32 kDrawRefracAndSpec         = 0x8027c12cu;  // TModelWaterManager::drawRefracAndSpec
constexpr u32 kCMTXLightPerspective      = 0x8034a17cu;  // C_MTXLightPerspective (SDK)

// ── Pure math (testable, no guest state) ─────────────────────────────────────────────
// Closed-form m00 of the effect matrix built by 0x8022ba74 (C_MTXPerspective row 0):
//   m00 = cot(fovy_deg/2) / aspect
inline float water_effect_m00(float fovy_deg, float aspect) {
    const float half_rad = fovy_deg * 0.5f * (3.14159265358979323846f / 180.0f);
    return (1.0f / std::tan(half_rad)) / aspect;
}
// Closed-form m00 of C_MTXLightPerspective(m, fovy, aspect, scaleS, …):
//   m00 = scaleS * cot(fovy_deg/2) / aspect       (drawRefracAndSpec passes scaleS=0.5)
inline float water_lightpersp_m00(float fovy_deg, float aspect, float scaleS) {
    return scaleS * water_effect_m00(fovy_deg, aspect);
}
// Aspect parameterization: factor applied to a 4:3-derived m00 to retarget it at the
// effective render aspect. Identity for guest-identical 4:3.
inline float water_aspect_factor(float guest_aspect, float effective_aspect) {
    return guest_aspect / effective_aspect;
}

// ── Mode / env ───────────────────────────────────────────────────────────────────────
bool native_on() {
    static const char* e = getenv("SUNBRIGHT_WATER_NATIVE");
    static const bool on = !(e && e[0] == '0');
    return on;
}
// Scale applied to lookup m00: always the live raster squeeze (raster m00 = guest m00 ×
// squeeze ⇒ lookup must match). At 4:3 the squeeze is 1.0, so guest-identical behavior
// falls out by construction — no mode flag. (Was SUNBRIGHT_WATER_WS=1 opt-in pending
// verification; user-verified in Delfino 2026-06-12, gate removed.)
float water_lookup_scale() {
    return ws_squeeze_scale();
}
bool dbg_on() {
    static const bool d = getenv("SUNBRIGHT_DBG_WATER") != nullptr;
    return d;
}

void run_orig(CPUState& cpu, u32 addr) {
    if (RecompFunc orig = recomp_raw(addr)) orig(cpu);
    else call_ppc(cpu, addr);
}

// Set while TModelWaterManager::drawRefracAndSpec runs (emu thread only — draw is
// single-threaded; same pattern as efbtex_widescreen.cpp).
bool g_in_water_refrac = false;

// ── Seam 1: the J3D effect-matrix helper (sea + all refraction materials) ───────────
SUNBRIGHT_OVERRIDE(ov_water_effectmtx, kGetLightPerspForEffectMtx) {
    const u32 out = cpu.gpr[3];
    run_orig(cpu, kGetLightPerspForEffectMtx);
    if (!native_on() || out < 0x80000000u) return;
    const float s = water_lookup_scale();
    if (s == 1.0f) return;                      // guest-identical mode: no memory touch
    const f32 m00 = mem_rf32(out);
    mem_wf32(out, m00 * s);
    if (dbg_on()) { static int n = 0; if ((n++ % 600) == 0)
        std::fprintf(stderr, "[water] effectmtx m00 %.5f -> %.5f (s=%.4f)\n", m00, m00 * s, s); }
}

// ── Seam 2: drawRefracAndSpec scope + its C_MTXLightPerspective TEXMTX0 ─────────────
SUNBRIGHT_OVERRIDE(ov_water_refrac_scope, kDrawRefracAndSpec) {
    if (!native_on()) { run_orig(cpu, kDrawRefracAndSpec); return; }
    g_in_water_refrac = true;
    run_orig(cpu, kDrawRefracAndSpec);
    g_in_water_refrac = false;
}

SUNBRIGHT_OVERRIDE(ov_water_lightpersp, kCMTXLightPerspective) {
    const u32 out = cpu.gpr[3];
    const bool scoped = native_on() && g_in_water_refrac && out >= 0x80000000u;
    run_orig(cpu, kCMTXLightPerspective);
    if (!scoped) return;
    const float s = water_lookup_scale();
    if (s == 1.0f) return;                      // guest-identical mode: no memory touch
    const f32 m00 = mem_rf32(out);
    mem_wf32(out, m00 * s);
    if (dbg_on()) { static int n = 0; if ((n++ % 600) == 0)
        std::fprintf(stderr, "[water] refrac lightpersp m00 %.5f -> %.5f (s=%.4f)\n", m00, m00 * s, s); }
}

} // namespace
