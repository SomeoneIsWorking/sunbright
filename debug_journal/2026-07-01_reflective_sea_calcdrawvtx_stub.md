# file-select reflective sea — ROOT CAUSE FOUND: `calcDrawVtx` is an un-decompiled stub

Continues `scratch/handoff_reflective_sea.md` (user directive: "native should reproduce the
reflective sea"). Previous session's lead — "DrawBuf Indirect is EMPTY → the indirect water model
never enters" — was investigated and **found to be a red herring**. The real cause is different and
now pinned by VALUE, not by eye.

## What the reflective sea actually IS (measured, not guessed)
The turquoise reflective/refractive water surface is drawn by **`TModelWaterManager`** (the
`水マネージャ` object, `reference/sms/src/Player/ModelWaterManager.cpp`), NOT by the `インダイレクトシーン`
list and NOT via `DrawBuf Indirect`.

The perform-list dispatch (SB_PL_DBG, decoded shift_jis) puts 水マネージャ in every phase and it is
`found=1` in native:
- `PerformList Movement`  水マネージャ 0x1  (move)
- `PerformList CalcAnim`  水マネージャ 0x2  (calc)
- `PerformList GX`        水マネージャ 0x8  (drawSilhouette/drawWaterVolume/drawMirror/drawShineShadowVolume)
- **`PerformList GX Post`  水マネージャ 0x80** → `TModelWaterManager::perform(0x80)` → **`drawRefracAndSpec()`** = the reflective sea (ModelWaterManager.cpp:1584).

`drawRefracAndSpec()` sets up **indirect texturing** (`GXSetTevIndWarp`, projective TEXCOORD0 from POS
via `C_MTXLightPerspective`) sampling the scene EFB copies (unk5D34/38/3C = screen/scene textures) and
draws **`unk5D30->draw()`** up to 3× (gated on `unk5D60` bits 2/4/8; unk5D60=0x16F has all three).
`unk5D30` is a **`TDLTexQuad`** — a GX display-list quad grid, NOT a J3DShape.

## The two gaps (both real; #1 is the show-stopper)
Instrumented `TModelWaterManager::perform` natively (`SB_WATER_DBG=1`, kept in-tree). Under the default
file-select measure run (SB_OWN_GXLIST=1, real perform list):
```
[water] perform(0x80) unk5D60=0x16f quad=0x… quadN=0 tex0=… tex1=… tex2=…
```
- **perform(0x80) DOES run** in native, drawRefracAndSpec IS reached, textures are non-null.
- **quadN = 0** — the TDLTexQuad grid is EMPTY, so `unk5D30->draw()` early-returns (`if (unk8 != 0)`),
  so NOTHING water-reflective draws. Same empty buffer kills drawSilhouette (also draws unk5D30).

**GAP #1 (root cause): `TModelWaterManager::calcDrawVtx(MtxPtr)` is an EMPTY STUB** —
`ModelWaterManager.cpp:763` is literally `{ }` (un-decompiled, `#pragma dont_inline`). calcDrawVtx is
what tessellates the visible water surface INTO the TDLTexQuad grid (the `26×52 = 1352`-vert plane the
oracle draws in pass3). Called from `perform(0x4)` (line 1568). Because it does nothing, `unk5D30` is
never `request()`-ed → quadN stays 0 → the whole water-reflective/refractive/silhouette draw path is
dead. ROM addr **`0x8027e5f4`** (spans to 0x8027e8e0 = 0x2EC = 187 instrs). Not decompiled anywhere in
reference/sms — must be RE'd from the ROM (Ghidra) and ported.

**GAP #2 (needed after #1): native capture does not tap the TDLTexQuad draw.** The native J3D capture
(`native/render/sms_boot_j3d_capture.cpp`) taps ONLY `J3DShape::draw` (`sb_boot_capture_j3d`).
`TDLTexQuad::draw()` (`reference/sms/src/MarioUtil/DLUtil.cpp:97`) issues geometry via
`GXCallDisplayList` over an indexed POS array (`unk14`, GX_INDEX16) + a fixed 8-byte UV — it never
touches J3DShape. So even once calcDrawVtx fills the grid, native's renderer won't SEE it until the
TDLTexQuad/GXCallDisplayList path is captured, WITH the indirect-texturing + EFB-scene-texture material
(the reflective look). This is a new capture path, similar in spirit to the J3D one.

## DEAD ENDS confirmed this session (do NOT re-chase)
- **`インダイレクトシーン` / `DrawBuf Indirect`** are NOT the reflective sea. Probed at runtime
  (`SB_IND_DBG=1`, scene_drive.cpp): インダイレクトシーン IS found and IS a
  `JDrama::TViewObjPtrListT<TViewObj>` (vtable resolved via nm) but has **child_count=0** (empty list);
  `SeaIndirect`/`ReflectParts`/`ReflectSky` are all NULL (not created in the file-select/option map).
  `DrawBuf Indirect` heads=0 packets=0. That whole branch is inert in file-select — the handoff's
  "enter the indirect water model into DrawBuf Indirect" plan targets the wrong mechanism.
- `TEmitterIndirectViewObj` (`EmitterIndirectViewObj`, found=0) is a PARTICLE emitter, unrelated.
- The `PERF…`-named perform entries showing `found=0` (PERFステージ/PERFマップ描画/…) are profiling
  markers, harmless; the actual group entries (マップグループ/水マネージャ/プレーヤーグループ) are found=1.

## NEXT STEPS (the port)
1. **Decompile `calcDrawVtx` @0x8027e5f4** (decomp-port / Ghidra headless) → readable C, port it into
   `ModelWaterManager.cpp:763` behind `SMS_NATIVE_PLATFORM` (keep the stub off-platform). It reads the
   water particle/area state + view mtx and `unk5D30->reset()/request()/setEnd()`s the quad grid.
   Verify with `SB_WATER_DBG` that quadN > 0 after perform(0x4).
2. **Capture the TDLTexQuad path.** Add a tap (native override of `TDLTexQuad::draw`, or intercept the
   `GXCallDisplayList` in gx) that decodes the indexed POS grid → tris into the capture, tagged with the
   reflective indirect-texturing material (indirect warp + EFB scene texture sample). Needs indirect/EFB
   texgen support in `sms_boot_material.cpp` / the TEV decode.
3. Measure: `scratch/run_measure.sh <tag>` (stopgap baseline 14.0; target = reflective sea rendered,
   image matches `scratch/passes/oracle_pass3_final.png` turquoise water). The ph6-MapXlu STOPGAP in
   JDRDrawBufObj.cpp is orthogonal — leave it until the sea draws, then re-evaluate.

## ⚠ CORRECTION (same session): the water-manager path is ALSO ruled out
Verifying before porting calcDrawVtx (per "believe the observation, verify") saved a wasted port.
Decompiled calcDrawVtx @0x8027e5f4 (Ghidra headless, `scratch/decomp/8027e5f4.c`): it builds ONE
TDLTexQuad per **water PARTICLE** with `mParticleFlagSOA[i]&0xf == 1`. Those particles are **FLUDD
water SPRAY droplets** (created by Mario spraying; `move()` advances them with gravity/velocity) — NOT
a static sea surface. Extended `SB_WATER_DBG` to count particles: file-select shows
**`nParticle=0 (type1=0 type2=0 type3=0)`** every frame. So even a fully-ported calcDrawVtx would draw
NOTHING in the idle file-select scene. **drawRefracAndSpec / the water manager is NOT the file-select
reflective sea.** (calcDrawVtx being a stub is still a real gap for in-GAME sprayed-water reflection —
worth porting later — but it is NOT this bug.)

## ⚠ ALSO ruled out this session (all EMPTY / never fire in file-select)
- **DrawBuf AfterIndirect Opa/Xlu**: heads=0 packets=0 (`SB_DRAWBUF_INV`, now includes them). The sea's
  flag-0x80 → AfterIndirect routing (TMapStaticObj::perform:209) would land here — it's empty.
- **The "sea"/"SeaIndirect"/"ReflectSky"/"ReflectParts" TMapStaticObj**: `SB_SEA_DBG` tap in
  TMapStaticObj::perform prints NOTHING → no static object with those names ever performs in the option
  map. So the file-select sea is NOT a named `sea` static object; it is part of the map model
  (マップグループ → DrawBuf MapOpa=7pkt / MapXlu=2pkt, which native DOES draw as the flat teal b9).

## So WHERE is the pass-3 reflective sea? (open — next session starts HERE)
All guessed native mechanisms are inert. Native draws the sea ONCE (b9, ph1/scene pass, ~1413 verts,
bm=NONE opaque, flat teal). The oracle draws an ADDITIONAL layer in the DISPLAY pass (pass3, ~1352
verts, tev=2, blend SRCALPHA/SRCCLR, persp). NOTE: the "indirect texturing" claim in the handoff was an
INFERENCE, not measured — the gxblend line only says tev=2 SRCALPHA/SRCCLR. So the reflective sea may
just be a **second, alpha-blended 2-TEV-stage draw of the sea geometry in the display pass** that native
never issues.

**NEXT (TOOLING-FIRST): get oracle-side attribution.** Use the GX command-stream oracle
(`build/sunbright`, pure Dolphin-GX, `runtime/gx_stream.cpp`/`gx_parse.h` — memory
[[gx-command-stream-oracle]]) to identify WHICH GC draw call / source function emits the pass3
1352-vert SRCALPHA/SRCCLR sea, and its material/texture. Stop guessing native mechanisms; read the
oracle's actual command stream for that draw. Then find why native omits that pass (likely: the sea
model's second material/pass, or a display-pass re-draw the native capture drops). Compare native b9's
material to the oracle's pass3 material at the VALUE level.

## Tooling added (gated, kept)
- `SB_WATER_DBG=1` — `TModelWaterManager::perform` per-flag trace + TDLTexQuad quadN + particle counts (ModelWaterManager.cpp).
- `SB_IND_DBG=1` — indirect-scene / reflect-object class+child probe (scene_drive.cpp, `sb_indirect_probe`).
- `SB_SEA_DBG=1` — TMapStaticObj::perform tap for sea/SeaIndirect/ReflectSky/ReflectParts (MapStaticObject.cpp).
- `SB_DRAWBUF_INV=1` — now also inventories DrawBuf AfterIndirect Opa/Xlu + StaticMapObj Sun Opa/Xlu.
- `scratch/decomp/8027e5f4.c` — decompiled calcDrawVtx (for the later in-game sprayed-water port).
