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

---

## SESSION N+6 (2026-07-01, continued) — RULED OUT 半透明優先 (verified unk0=0); reflective sea = a **1352v PROCEDURAL grid** absent from native; corrects the stopgap's "ph6 MapXlu re-entry = 1352v model" claim

Measured, not guessed. Two new value probes drove this:
- `SB_DBHEAD_PKT` extended (J3DDrawBuffer.cpp): now prints **per-packet** tev-stage-count + blend(mode/src/dst) + tevColor reg1, for every flush of the MapXlu buffer (ph1 AND ph6).
- `SB_XLU_DBG` new (MapXlu.cpp): dumps `TMapXlu::init` stream pos/len + raw bytes + the read `unk0` (xlu-priority group count).

### FINDING 1 — native's MapXlu buffer holds ONLY foam + mask; NEITHER is the reflective sea (VALUE-proven)
`SB_DBHEAD_PKT` at settled file-select, the MapXlu buffer (b1f7bc) flushed in BOTH phase 1 and phase 6 holds the SAME 2 packets:
- pkt A `mat=c97468` (**foam**): **tev=1**, blend=1/4/5 (BLEND/SRCALPHA/INVSRCALPHA), reg1=255,255,255,255. Native batch **b11**: vc=**30**, a=0.5, r0=30,50,115,180 (dark blue).
- pkt B `mat=c97c48` (**mask**, key eb5c8e74): **tev=3**, blend=1/4/2 (BLEND/SRCALPHA/SRCCLR), reg1=0,0,4,89. Native batch **b12/b76**: vc=**15**, huge grazing plane (ndcX ±100000, uv tiled 84×), r0=175,240,240,146. = THE overbright wash.

The oracle's reflective-sea draw (`oracle_gxdbg.log` draw#2906) is **tev=2**, blend SRCALPHA/SRCCLR, **reg1=194,242,190** (turquoise), **1352 verts** (26 draws × 52). ⇒ neither MapXlu packet matches: wrong tev count (1 or 3, not 2), and 30/15 verts vs 1352. **The reflective sea is NOT in native's MapXlu buffer.**

⛔ **CORRECTS the JDRDrawBufObj.cpp stopgap comment** ("the GXPost/display pass RE-ENTERS MapXlu with the reflective water-surface model, a tev=2 ~1352-vert draw"). WRONG: the ph6 MapXlu re-entry draws the SAME foam+mask (15v+30v), not any 1352v model. The wash is purely the **ph6 mask (c97c48, 15v) drawn colorUpdate=TRUE**; the reflective sea is a SEPARATE 1352v object drawn ELSEWHERE that native lacks entirely.

### FINDING 2 — `DrawBuf Map 半透明優先` (translucency-priority) is LEGITIMATELY empty (unk0=0, VERIFIED) — RULED OUT
The GX-Post perform list (`MarDirectorPreEntry.cpp:39-44`) has `DrawBuf Map 半透明優先 (opa/xlu)` + `半透明優先2` buffers, populated by `マップ`(bare TMap) performs 0x4000200 / 0x2000200 → `TMap::perform` → `mXlu->changeXluJoint(0/1)` (Map.cpp:184-192). But `SB_MAPXLU_DBG` shows `xluCount=0` and `SB_XLU_DBG` shows:
```
[xlu-init] pos=17 len=45 bytes=00000000 00000028 00000028 00002ee0 000061a8 000007d0 00000000
[xlu-init] unk0(numGroups)=0
```
`unk0=0` ⇒ changeXluJoint(n) returns false for all n (n>=unk0) ⇒ `TMap::perform` early-returns ⇒ nothing enters 半透明優先. And this is **CORRECT, not a stream misread**: the bytes after unk0 (0x28=40, 0x28, 0x2ee0=12000, 0x61a8=25000, 0x7d0=2000) are the collision-grid extents correctly consumed next by `mCollisionData->init` (which works — Mario floor/death-plane debugged fine). The option map genuinely has 0 xlu-priority groups. **半透明優先 is empty on the GC too. RULED OUT.** (Same on the oracle.)

### FINDING 3 — the reflective sea is a **PROCEDURAL 1352v grid** (26 draws × 52 verts), i.e. dynamic tessellation
26 separate draws of exactly 52 verts each = a procedurally-generated grid (26 rows), NOT a static BMD shape (which would be 1 draw / a few strips). This is the signature of a **TDLTexQuad-style screen-projected water grid**. The water-manager TDLTexQuad is ruled out (calcDrawVtx is particle-driven, particles=0 — re-confirmed by re-reading `scratch/decomp/8027e5f4.c`: loops `iVar6 < particleCount@+0x12`, flag `&0xf==1`). So a DIFFERENT procedural water-grid renderer produces the sea, and native never runs it.

### Native side, for reference
- `b9` = the flat sea native DOES draw: **DrawBuf MapOpa**, ph1, **bm=0/1/0 OPAQUE**, 1413v, rgb 0.47,0.85,0.76, 256×256 tex tiled ~65×, reg1=255,255,255,255. Its material's J3DBlend (read via `pe->getBlend()` in sms_boot_material.cpp) is genuinely opaque — so on the GC the reflective look is NOT b9; b9 is the opaque base water. The 1352v translucent grid is additive on top.

### NEXT (still tooling-first, but now much narrower)
The reflective sea = a procedural ~1352v (26×52) SRCALPHA/SRCCLR tev=2 turquoise water grid drawn in GX Post, absent from native. Two concrete leads to decide between:
1. **The `sea` TMapStaticObj** (MapStaticObject.cpp:58, model="sea", parent="マップグループ", flag 0x80 → AfterIndirect). `SB_SEA_DBG` (tap in `TMapStaticObj::perform`) printed nothing → either not created in the option map, OR it performs via a non-TMapStaticObj vtable / the tap misses it. **Re-verify by tapping object CREATION** (genObject/the actor-data-table walk), not just perform. If created, read whether its draw is the 1352v TDLTexQuad grid.
2. **Geometry-bbox oracle**: add vertex-position decode to `gx_parse.cpp` `OnPrimitiveCommand` (the `const u8*` vtx-data arg is currently ignored) → dump the world/NDC bbox of the pass3 SRCALPHA/SRCCLR draw → match it to a native object's bbox. This ends the "same-geometry-as-b9 vs separate-object" question definitively.

### Tooling added this session (gated, kept, committed)
- `SB_XLU_DBG=1` — `TMapXlu::init` stream pos/len/bytes + unk0 (MapXlu.cpp).
- `SB_DBHEAD_PKT=1` — now also prints per-packet tev-stage-count + blend(mode/src/dst) + reg1 (J3DDrawBuffer.cpp).

---

## SESSION N+7 (2026-07-01) — the option-map reflective object = `sun_mirror`; TWO game-wide host/decomp bugs FIXED (static-obj table lookup + material-anm host-pointer guard). Sea reflection now un-crashed but still needs its EFB reflection source.

### Attribution: only ONE static object exists in the option map — `sun_mirror`
Ruled out every draw-buffer mechanism (N+6). The reflective sea (pass3, 1352v, SRCALPHA/SRCCLR tev=2
turquoise) is NOT a map/MapXlu/半透明優先/Indirect/water-mgr draw. Found the reflective SURFACE class
`TShimmer` (Map/Shimmer.cpp) — a screen-texture (`スクリーンテクスチャ`) projective-reflection water — but
`SB_SHIMMER_DBG` shows `TShimmer::perform` **never runs** in file-select (インダイレクトシーン child_count=0).
New tap `SB_STATICOBJ_DBG` (TMapStaticObj::init) shows the option map creates exactly **one** TMapStaticObj:
`sun_mirror` (the sun/sky reflection on the water = the bright turquoise shimmer band the oracle shows).

### BUG #1 (game-wide) — `TMapStaticObj::init` name→config table lookup had the exit test INVERTED
`Map/MapStaticObject.cpp:322`: `for(i=0;;++i){entry=&table[i]; if(strcmp(name,table[i].unk0)) break;}`
breaks on the first MISMATCH, so EVERY static object resolved to `table[0]` ("SeaIndirect", unk40=0x41).
PROVEN: `sun_mirror` (real unk40=0x62) came back **0x41** (`[staticobj] created name='sun_mirror'
unk40=0x41`). Consequence: sun_mirror loaded SeaIndirect's config → wrong model, no
`entryMirrorDrawBufferAlways` (0x20), no model-load path (0x2) → its reflection never set up (silently,
via the `&& unk70` draw guards). FIX: break on MATCH (`strcmp(...)==0`), fail-fast OSPanic on the
nullptr terminator. Verified: `sun_mirror` now `unk40=0x62`, loads `/common/map/sun_mirror.bmd`.

### BUG #2 (game-wide, the big one) — `J3DMaterial::getMaterialAnm()` GC pointer-range guard nulls ALL host anms
Fixing #1 exposed a SIGSEGV in `SMS_CalcMatAnmAndMakeDL` → `mat->getMaterialAnm()->calc()` (null anm).
`SB_MATANM_DBG` (new taps in MActor::setModel + updateMatAnm) showed setModel STORING a valid anm
(`setMaterialAnm(0x7fff…)`) but `getMaterialAnm()` returning `(nil)` immediately after. ROOT CAUSE:
`J3DMaterial.hpp:81 getMaterialAnm()` = `if ((uintptr_t)unk38 < 0xC0000000) return unk38; else return nullptr;`
— a GameCube pointer-sanity filter (valid GC RAM = 0x8xxxxxxx < 0xC0000000). On the 64-bit host every
real pointer is `0x7fff_xxxx_xxxx` which is **> 0xC0000000**, so the guard nulled EVERY anm ever set →
**all material animations across the whole port were silently dead**, and any updateMatAnm path
NULL-derefed. The J3DMaterial ctor inits `unk38=nullptr` and setMaterialAnm only stores null/valid, so
the filter is unnecessary on host. FIX (`#ifdef SMS_NATIVE_PLATFORM`): `return unk38;` directly.
VERIFIED: `[matanm-update] … anm=0x7fffe3e8c908` (non-null), null-anm fail-safe fires **0 times**, no crash.

### Result + what's STILL missing (the reflection source)
With both fixes: file-select renders crash-free, sun_mirror loads and its material anims run
(scene_verts 30099). BUT the sea is still flat teal (region RGB 79,151,164 vs oracle 114,184,194) — the
overbright number is unchanged (14.0, the ph6-MapXlu stopgap baseline). So sun_mirror DRAWS but its
reflection isn't visible: it enters `DrawBuf MirrorAlways` (unk40&0x20) and samples a mirror/EFB
reflection texture that is EMPTY in native (native's mirror/screen-texture EFB capture is the known-
incomplete infrastructure — memory notes on the empty 256×256 mirror EFB). NEXT: wire sun_mirror's
reflection source — either the MirrorAlways-buffer mirror-EFB, or (if sun_mirror uses スクリーンテクスチャ)
the screen-texture copy — so its projective-texgen material samples the real reflected scene. THEN the
bright turquoise shimmer appears and the ph6-MapXlu stopgap can be re-evaluated.

### Tooling added this session (all gated, kept)
- `SB_STATICOBJ_DBG` — TMapStaticObj::init: per-created-object name + unk40 + the `unk1C` list-insert TODO.
- `SB_MATANM_DBG` — MActor::setModel + updateMatAnm per-material unk30/unk2C/anm-ptr trace.
- `SB_SHIMMER_DBG` (pre-existing) — confirmed TShimmer::perform never runs in file-select.
- Defensive null-anm fail-safe in `SMS_CalcMatAnmAndMakeDL` (draws-without-anm + one-shot WARN).
