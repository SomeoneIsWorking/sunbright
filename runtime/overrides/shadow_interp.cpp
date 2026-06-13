// 60fps interpolation — Mario's REAL cast shadow (TMBindShadowManager, the bind-shadow system).
//
// STATUS (2026-06-13): no override active — being re-approached with the Mario-only debug room
// (SUNBRIGHT_DEBUGROOM=1, a secret/_ex stage with only Mario, so the shadow can be isolated).
//
// RE so far (binary; ShadowUtil.cpp is not in the decomp):
//   TMBindShadowManager::perform(param, gfx) @ 0x80231108 (whole fn 0x80231108..0x802311cc):
//     param & 0x4         -> calcVtx 0x8022e0cc: build draw geometry IN PLACE into the stable,
//                            heap-allocated (load 0x80231288) buffer *(this+0x1c), count @ +0x20,
//                            from gameplay requests + view matrix 0x804045dc.
//     param & 0x8         -> if global[r13-0x60f7] != 0  drawShadowGD 0x8022fa40 (display list)
//                            else                        drawShadow   0x8022f014 (immediate).
//     param & 0x20000000  -> FINALIZE: zero the count words (+0x14, +0x20, +0x2c, +0x40).
//   Requests are produced ONLY in TMarDirector::direct Branch A (calc), which the 60fps in-between
//   skips; the +0x20 silhouette perform list IS re-issued on the in-between (so perform is called
//   there). The real cast shadow is drawn via the GD (display-list) path.
//
// RULED OUT (all rebuilt/tested, each wrong):
//   * restore the REQUEST count, let calcVtx rebuild  -> wrong/empty shadow (requests gone).
//   * also copy the request bytes each field          -> fixed data but the per-byte copy = lag.
//   * persist the draw count + re-draw via perform &8 -> still flickered (GD vs immediate path mix).
//   * force the IMMEDIATE path on every field         -> NO shadow + heavy lag (the cast shadow is
//                                                        the GD path; immediate drawShadow is not a
//                                                        standalone substitute here).
// Next: with the isolated debug room, RE the GD (display-list) path — how drawShadowGD builds and
// where its display list lives across fields — and re-draw THAT on the in-between (one path: GD),
// or port the GD shadow draw natively. No override is registered until that is correct.
#include "../overrides.h"
#include "../intrinsics.h"
