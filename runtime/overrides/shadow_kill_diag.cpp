// DIAGNOSTIC (default OFF) — suppress Mario's cast-shadow draw to falsify the "floor wash = missing
// cast-shadow darkening" hypothesis (debug_journal/2026-06-18_floor_wash_NOT_combiner.md).
//
// The cast shadow (TMBindShadowManager) is an EFB DESTINATION-ALPHA STENCIL darkening pass that ngx
// (object-model renderer) never sees — so ngx's floor stays bright while Dolphin-GX darkens it.
// HYPOTHESIS: that darkening is THE floor wash. TEST: no-op the shadow draw in BOTH oracle_ab runs.
//   - oracle (Dolphin GX): currently draws the shadow -> floor dark. Killing it -> floor goes bright.
//   - ngx: never drew it -> unchanged.
// => if the floor A/B delta DROPS with SUNBRIGHT_KILL_SHADOW=1, the cast shadow IS the missing draw.
// If it does NOT drop, the darkening is something else (re-localize). This is a falsifiable probe,
// NOT the fix; the fix is to capture+composite the shadow natively (own-it-natively).
//
// gate: SUNBRIGHT_KILL_SHADOW=1. Default off => not registered => original runs under Dolphin JIT.
#include "../overrides.h"
#include <cstdlib>

#ifdef HAVE_DOLPHIN_CORE

static bool kill_shadow_on() {
    static int v = -1;
    if (v < 0) v = getenv("SUNBRIGHT_KILL_SHADOW") ? 1 : 0;
    return v == 1;
}

// SUNBRIGHT_KILL_SHINESHADOW=1 — no-op TModelWaterManager::drawShineShadowVolume (the Delfino-plaza
// POLLUTION darkening: concentric sphere volumes building an EFB-alpha mask, then a dark-blue (0,20,40)
// masked blend over the ground; map 1 only, recedes as flag 0x40000 rises). /gxblend named it as the
// SUBTRACT (cnt~1635) darkening pass. TEST: kill it in the ORACLE — if the floor goes bright (==ngx),
// it is THE ground wash. Then port it natively into ngx (own-it-natively); this probe is NOT the fix.
static bool kill_shineshadow_on() {
    static int v = -1;
    if (v < 0) v = getenv("SUNBRIGHT_KILL_SHINESHADOW") ? 1 : 0;
    return v == 1;
}

// SUNBRIGHT_KILL_MAPOBJWAVE=1 — no-op TMapObjWave::perform (the Delfino plaza water/wave draw, "波の表現").
// /gxblend named its initDraw (0x801dd720) as the BLEND src=SRCALPHA dst=SRCCLR (multiply-ish) darkening
// caller (cnt 326). HYPOTHESIS: the wave draws a multiply-darkening over the plaza ground that ngx misses
// (the residual floor gap: ngx 129 vs oracle 101, after the pollution port). TEST: kill it in the ORACLE
// — if the floor brightens toward ngx (129), the wave IS the residual darkening; then port it natively.
static bool kill_mapobjwave_on() {
    static int v = -1;
    if (v < 0) v = getenv("SUNBRIGHT_KILL_MAPOBJWAVE") ? 1 : 0;
    return v == 1;
}

namespace {
// drawShadowGD__19TMBindShadowManagerFUlPQ26JDrama9TGraphics — the EFB stencil stamp + composite.
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_kill_drawShadowGD, 0x8022fa40u, kill_shadow_on()) {
    (void)cpu;  // no-op: skip the cast-shadow EFB darkening pass entirely.
}
// drawShadow__19TMBindShadowManagerFUlPQ26JDrama9TGraphics — the non-GD variant of the same.
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_kill_drawShadow, 0x8022f014u, kill_shadow_on()) {
    (void)cpu;
}
// drawShineShadowVolume__18TModelWaterManager — the Delfino plaza pollution darkening.
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_kill_shineshadow, 0x8027c67cu, kill_shineshadow_on()) {
    (void)cpu;  // no-op: skip the shine-shadow-volume pollution darkening entirely.
}
// perform__11TMapObjWave — the plaza wave/water draw (calls initDraw + draw). No-op skips both.
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_kill_mapobjwave, 0x801dcdd8u, kill_mapobjwave_on()) {
    (void)cpu;  // no-op: skip the wave draw entirely.
}
}  // namespace

#endif  // HAVE_DOLPHIN_CORE
