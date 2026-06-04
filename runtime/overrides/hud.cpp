// Native HUD ownership — Super Mario Sunshine in-game HUD (TGCConsole2) — STATUS / RE LOG.
//
// GOAL: own the in-game HUD's layout and render it widescreen (corner clusters anchored to the 16:9
// edges, correct aspect) instead of the centred/inset look the global 2D squeeze gives.
//
// WHAT THE RE ESTABLISHED (so the next attempt doesn't re-walk it):
//  • The HUD is drawn by TGCConsole2::perform (USA 0x8014083c, recompiled). It draws every element —
//    counter digits/icons/bars, lives, the FLUDD water gauge, the health sun — as 2D quads.
//  • Each 2D quad is positioned by VERTEX coordinates, not a matrix: the position matrix loaded for
//    2D is IDENTITY (verified — all 2D GXLoadPosMtxImm matrices are m03=0, identity rotation). So
//    this+0x54 leaf matrices and gx0/argX in drawSelf are NOT the lever (measured: leaf m03 shift =
//    no change; every HUD element has this+0x134==1, which makes drawFullSet take an alignment path
//    that ignores gx0+argX).
//  • The actual screen coordinates are the corner floats computed in J2DPicture::drawFullSet
//    (0x802cc838) and handed to the quad emitter 0x802cd2ec. THAT is the true position lever.
//
// WHY RUNTIME OVERRIDES CAN'T REACH IT (the wall):
//  • The scene director that dispatches perform, and the J2D draw pipeline below drawSelf
//    (drawFullSet → 0x802cd2ec), run in Dolphin's JIT via block-linking, which bypasses our
//    __wrap_JitTrampoline hook (it only fires at block dispatch, not linked jumps).
//  • And the recompiler did NOT wire those calls as recomp→recomp: func_802cd2ec exists but NOTHING
//    in generated/ ever call_ppc's it (grep: zero references). drawFullSet's tail (the 0x802cd2ec
//    calls) isn't emitted as a recomp call. So even forcing drawSelf through recomp doesn't reach it.
//  • Net: override_lookup is never consulted for the quad emitter, so no runtime override can move
//    the HUD quads. (Overrides DO fire for the trampoline entry points — drawSelf 0x802cc7c0,
//    J2DPicture::draw 0x802ccef4 — but those only carry the IDENTITY matrix / ignored-gx0 args.)
//
// THE REAL PATH TO OWN THE HUD (recompiler-level, needs a /recompile — not a runtime override):
//  1. Fix the CFG/function-boundary handling so drawFullSet (0x802cc838) and the quad emitter
//     0x802cd2ec are fully recompiled and CALLED via call_ppc (so override_lookup applies); OR
//  2. Emit an override hook at recompile time for the quad emitter; then a small native override can
//     classify each element by `this`+0x10 name and shift the corner-float X to the 16:9 edge.
//  Either way the per-element classifier already worked out (s_/d_/c_=left, m_/go=centre, w_t0=right,
//  exact role gate so menus are untouched) drops straight in once the emitter is reachable.
//
// Until then the HUD stays at the correct-aspect-but-inset look from the global 2D squeeze
// (scene_render.cpp ov_gx_projection) — no crashes, menus unaffected.

// g_2d_active: set by scene_render's GXSetProjection hook (current projection is 2D ortho). Kept as
// the wiring is in place for the recompiler-level fix above; no hot override is registered here.
bool g_2d_active = false;
