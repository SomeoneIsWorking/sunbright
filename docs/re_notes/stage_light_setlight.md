# Stage GX-light load — TLightCommon::setLight (RE'd 2026-06-30)

The real SMS stage-light GX load, decompiled from the US DOL (GMSE01) because the community
decomp (`reference/sms` MarioUtil/LightUtil.cpp) left `TLightCommon::setLight`, `getLightColor`,
`getLightPosition`, `getAmbColor`, `loadAfter`, `perform` as **empty stubs**. This is the function
that actually loads the stage's GX lights — NOT the JDrama `TLightMap`/`TActor::issueGXLight` path
(both inert for the option scene) and NOT the `TLightAry` "Light Group" (that's just the palette).

## Addresses (GMSE01, from reference/sms_gmse01_funcs.txt)
| addr | symbol |
|---|---|
| 0x80229a30 | `setLight__12TLightCommonFPCQ26JDrama9TGraphicsi` |
| 0x80229610 | `setLight__11TLightMarioFPCQ26JDrama9TGraphicsi` (byte-identical to above) |
| 0x802298fc | `perform__12TLightCommon` (flag&0x80 → load same light into slots 0/1/2; flag&0x20 → setLight) |
| 0x80229880 | `perform__11TLightMario` (flag&0x20 → setLight(g, *(s16*)(r13-0x6098))) |
| 0x80229d78 | `getLightColor__12TLightCommon` |
| 0x80229ca0 | `getLightPosition__12TLightCommon` |
| 0x80229cec | `getAmbColor__12TLightCommon` |
| 0x80229e30 | `loadAfter__12TLightCommon` (caches 4 Light-Group + 2 Ambient-Group entries from unk24/unk20) |
| 0x80228490 | `loadAfter__22TLightWithDBSetManager` (fills gpLightManager effect data from Light-Group[0]) |
| statics (JP names) | `mLightAry`/`mAmbAry`/`mLightPos` of `TLightCommon`; `gpLightManager` |

GX helpers used: `GXInitLightPos` 0x8035f130, `GXInitLightColor` 0x8035f214, `GXInitLightAttn`
0x8035f024, `GXInitLightAttnA` 0x8035f040, `GXInitLightDistAttn` 0x8035f060, `GXInitSpecularDir`
0x8035f140, `GXLoadLightObjImm` 0x8035f26c, `GXSetChanAmbColor` 0x8035f3b4, `PSMTXMultVec`
0x8034a2d0, `PSVECNormalize` 0x8034a5d0.

## setLight(graphics, idx) — loads up to THREE GX lights
Reads ONE Light-Group entry (`getLightPosition(idx)`/`getLightColor(idx)` index Light-Group
`[idx + this->unk24]`; option scene uses the white sun at index 0):

1. **GX_LIGHT0 (id 1, ALWAYS)** — positional. `PSMTXMultVec(graphics->mViewMtx, sunPos)` →
   `GXInitLightPos`; colour `getLightColor(idx)`; flat attn `(a=1,0,0 k=1,0,0)`. `GXLoadLightObjImm(.,1)`.
2. **GX_LIGHT1 (id 2, IFF `gpLightManager->unk54 && gpLightManager->unk55`)** — the effect/specular
   light. Position `PSMTXMultVec(view, gpLightManager->unk1c..)`; colour = `gpLightManager->unk18`
   scaled by `unk28`. `GXInitLightAttnA` + `GXInitLightDistAttn`. `GXLoadLightObjImm(.,2)`.
   gpLightManager loadAfter fills unk18/1c/20/24 from **Light-Group[0]**.
3. **GX_LIGHT2 (id 4, ALWAYS)** — directional. `PSVECNormalize(sunPos)` then NEGATE →
   `GXInitSpecularDir`; colour `getLightColor(idx)`; specular attn. `GXLoadLightObjImm(.,4)`.

Then `GXSetChanAmbColor(GX_COLOR0A0, getAmbColor(idx))`.

So a stage with the effect light enabled loads **3 GX lights into slots 0/1/2**, all sharing the
Light-Group[0] colour. For the option scene (stage 15, title + file-select) that colour is white →
**3 white lights** — exactly what the GX-command-stream value oracle measures.

## Port + verification
- `native/render/sms_boot_setlight.h` — pure (Dolphin/GX-free) port of the selection math
  (`sb::build_stage_lights`), unit-tested in `native/platform/tests/setlight_test.cpp`.
- `native/src/scene_drive.cpp` drives it each frame before `perform(0x8)`, replacing the old
  8-Light-Group-entry blast (a pre-oracle guess; commit 0dff27f).
- VALUE-VERIFIED via `tools/render/title_value_oracle.sh`: light count 8→3, then
  `parity_sweep diff` reports light count **3.0 vs 3.0 (relΔ 0.0)**, ambient/material unchanged.

## Open / refinement
`effectOn` (the GX_LIGHT1 gate `gpLightManager->unk54 && unk55`) is set by an **inlined
calcLightBorder** not yet located (no standalone symbol). Native's `gpLightManager` ctor defaults
those flags OFF, so we currently drive the effect light ON (oracle-proven for file-select) from
Light-Group[0]; `SB_NO_EFFECT_LIGHT` A/Bs it off. To make this fully faithful for ALL stages
(some may disable it), find calcLightBorder and source the gate from the live manager.
