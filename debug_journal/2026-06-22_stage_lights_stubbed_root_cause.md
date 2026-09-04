# 2026-06-22 — Stage lighting root cause: it's NOT the lightmap, it's the stubbed TLightCommon/Shadow/Mario

## TL;DR (corrects the s26 handoff + memory)
The s26 handoff/memory `native-lighting-consumer-and-gx-dl-keystone` proposed the bounded path
"the JDrama lightmap (`TLightMap`, count=0) is probably a load bug; fix `TLightMap::load` and
`TLight::perform(0x20)` populates GX slots 0/1." **That is wrong.** `mLightInfoCount==0` is the
GENUINE scene state — the Delfino lightmap legitimately has zero lights. Verified:
- `JSUInputStream::readS32` byte-swaps correctly on `SMS_NATIVE_PLATFORM` (`__builtin_bswap32`),
  so the count is read faithfully = 0.
- `TSmJ3DScn::loadSuper` DOES run (`mLightMap` non-null) and `TLightMap::load` reads count=0.

## The real root cause (MEASURED via the recomp disassembler)
The stage's actual lights are the scene objects **`normalLight` (TLightCommon)**,
**`shadowLight` (TLightShadow)**, **`MLight` (TLightMario)** — created by `TMarNameRefGen::getNameRef`
(MarNameRefGen.cpp). Their `perform` / `setLight` / `getLight*` are **EMPTY STUBS** in
`reference/sms/src/MarioUtil/LightUtil.cpp` (the community decomp never implemented them). So in the
native build NOTHING calls `GXLoadLightObjImm`/`GXSetChanAmbColor` → GXState lights stay empty →
`[light] nlights=0 loads=0` → the (already-wired, already-tested) per-vertex lighting consumer is inert.

`grep GXLoadLightObjImm reference/sms/src` → only JDRLighting.cpp (lightmap path, empty here),
DrawUtil.cpp (Mario silhouette), MapObjPlane.cpp (specific planes), GXLight.c (the impl). None load
the general stage sun, because that path (TLightCommon) is stubbed.

## The data IS available — only the GX-load glue is missing
RE'd the original PPC (addresses below; full spec in `scratch/light_re_spec.md`). `TLightCommon::loadAfter`
(0x80229e30) searches the scene NameRef tree for two objects by name:
- **"Light Group"** → `TLightAry` (genObject "LightAry", JDRNameRefGen.cpp) — `mLights[mLightCount]`
  of `TIdxLight` (size 0x6c). Each `TIdxLight : TLight` already carries a fully-loaded `GXLightObj`
  at +0x24 (color set by `TLight::load` → `GXInitLightColor`) and a position at +0x10
  (`TPlacement::mPosition`). `TLightAry::load` IS decompiled and runs.
- **"Ambient Group"** → `TAmbAry` (genObject "AmbAry") — `mAmbColors[mAmbColorCount]` of `TAmbColor`
  (size 0x18, `mColor` at +0x14). Decompiled + runs.

So the stage light/ambient DATA loads fine; the stubs that READ it and push it into GX_LIGHT0/1 +
the COLOR0A0 ambient register are what's missing.

## Original behavior to port (from scratch/light_re_spec.md)
- `TLightCommon::setLight(g, index)` (0x80229a30): transform `getLightPosition(index*2)` by the view
  matrix (`PSMTXMultVec`), `GXInitLightPos` + `GXInitLightColor`(from `getLightColor`) + `GXInitLightAttn`,
  `GXLoadLightObjImm(&obj, GX_LIGHT0=1)`. A gated 2nd light → **GX_LIGHT1=2** (gate =
  `gpLightManager->unk54 && unk55`). A specular light → GX_LIGHT2=4. Ambient →
  `GXSetChanAmbColor(GX_COLOR0A0, getAmbColor(...))`.
- `TLightCommon::perform` (gate `flags & 0x80`) fills slots 0/1/2 with the table-derived sun directly.
- `TLightShadow::perform` (gate `flags & 0x20`) → `setLight(g, 1)`. `TLightMario::perform`
  (gate `& 0x20`) → `setLight(g, <s16 global index>)`.
- Slot IDs are FIXED immediates (`li r4, 1/2/4`), NOT computed from index. Confirmed slots 0+1 are
  the diffuse-sun targets → matches the stage CLOF materials' `cc0=0x68e` (lights 0+1 enabled).
- Getters branch on flags `this[0x41]` (position embedded@0x44 vs table) / `this[0x28]` (color/amb).
  Table path: `entry = group->mLights[(index+unk24)*stride]`, light color via `GXGetLightColor(&entry+0x24)`,
  ambient via `tableA[...]+0x14`, with an alpha rescale by a `this` float.

### Open items before a fully-faithful port (spec §8)
- 6 SDA2 float consts at `r2+0xe850..0xe894` (attn/color unity) — dump from live DOL.
- The exact `index*2` vs `index` arg to the 3 setLight position/color reads.
- `r13`/`r2` absolute bases (globals given as `r13-0x61XX`).

## Pragmatic owned approach (verify-first)
Per the user directive ("PC port, own it, not bit-exact GC emulation"), the planned increment is to
drive a native port of the light load from scene_drive: find "Light Group"/"Ambient Group", load each
light into GX_LIGHT0/1 (view-space transformed, with its loaded color) + ambient from "Ambient Group".
The lighting CONSUMER (`sms_boot_lighting.h`) is already wired + tested and fires the moment GXState
slots 0/1 + ambient populate. A one-shot probe was added to `scene_drive.cpp`
(`[stage-light] LightGroup=.. count=.. / AmbGroup=.. count=..`) to confirm the runtime data BEFORE
porting (tooling-first). [RESULT: pending the run.]

## Key addresses (US disc GMSE01, reference/sms_gmse01_funcs.txt)
- 802298fc TLightCommon::perform · 80229a30 setLight · 80229ca0 getLightPosition · 80229cec getAmbColor
  · 80229d78 getLightColor · 80229e30 loadAfter · 80229fbc ctor
- 802298c0 TLightShadow::perform · 80229880 TLightMario::perform · 80229610 TLightMario::setLight
- Disassemble the listed addresses with the current PPC RE tooling; use
  `reference/sms_gmse01_funcs.txt` to resolve branch targets.
