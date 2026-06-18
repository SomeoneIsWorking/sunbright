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

namespace {
// drawShadowGD__19TMBindShadowManagerFUlPQ26JDrama9TGraphics — the EFB stencil stamp + composite.
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_kill_drawShadowGD, 0x8022fa40u, kill_shadow_on()) {
    (void)cpu;  // no-op: skip the cast-shadow EFB darkening pass entirely.
}
// drawShadow__19TMBindShadowManagerFUlPQ26JDrama9TGraphics — the non-GD variant of the same.
SUNBRIGHT_OVERRIDE_IF_NATIVE(ov_kill_drawShadow, 0x8022f014u, kill_shadow_on()) {
    (void)cpu;
}
}  // namespace

#endif  // HAVE_DOLPHIN_CORE
