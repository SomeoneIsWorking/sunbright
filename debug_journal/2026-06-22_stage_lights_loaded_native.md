# 2026-06-22 — Stage lighting LOADED natively (owned the stubbed TLightCommon path)

Continues `2026-06-22_stage_lights_stubbed_root_cause.md`. That session proved the missing
stage lighting is the **stubbed TLightCommon/Shadow/Mario** (their `perform`/`setLight` are
empty in `reference/sms/src/MarioUtil/LightUtil.cpp`), so nothing ever calls
`GXLoadLightObjImm` → the wired+tested per-vertex lighting consumer was inert (`nlights=0`).

## What landed (approach B — drive the load from scene_drive, own the path)
`native/src/scene_drive.cpp`: each frame, before `scene->perform(0x8)`, search the NameRef tree
for **"Light Group"** (TLightAry) and load light `i` → `GX_LIGHT(i)`:
- colour = `GXGetLightColor(&TIdxLight.unk24)` (loaded by `TLight::load`)
- position = `TPlacement::mPosition` view-transformed by `g_graphics.mViewMtx` (GX lighting is
  view-space), via `PSMTXMultVec`
- attenuation = flat `GXInitLightAttn(1,0,0,1,0,0)` (matches the TLight ctor)
- `GXLoadLightObjImm(&obj, GX_LIGHT0<<i)`
Plus `GXSetChanAmbColor(GX_COLOR0A0, AmbGroup[0])` from **"Ambient Group"** (TAmbAry).

This uses the already-native GX SDK functions (gx_impl.cpp) — same path the game would, just
driven from scene_drive instead of the stubbed class. The consumer (`sms_boot_lighting.h` →
`ngx::light_color0`) reads `sb_gx_get_lights()` during the shape draw and fires.

### Index→slot mapping decision (RE flagged ambiguity, resolved)
The binary has TWO models: `setLight` loads distinct lights per slot (`getLightColor(i)`→slot),
`perform` loads the SAME obj into all 3 slots. The latter would double-count light[0]. I use the
**setLight model** (`GX_LIGHTi ← light[i]`). On Delfino the palette is **L0=white, L1=black**, so
this yields a single effective white diffuse light — clean, no double-white. Documented in-code.

### Ambient correctness fix (faithful GX, not a bandaid)
`[light] amb0=0,0,0`: the stage CLOF material has **no ambient block** (`getAmbColor`→null), so the
capture defaulted ambient to 0 → with SIGN diffuse, back-facing verts clamped to **pure black**.
GX semantics: `cc0=0x68e` bit6=0 ⇒ ambSrc=**register**, so ambient must come from the
`GXSetChanAmbColor` register (= the AmbGroup the loader sets), NOT the material block. Fix:
- `sms_boot_j3d_capture.cpp`: track `MatEntry.hasAmb[c]` (true only when the material carries an
  ambient block); the draw loop uses the material block iff `hasAmb`, else the global register.
- `gx_impl.cpp`: new `sb_gx_get_chan_amb(slot, rgb[3])` export reads `GXState.chan[].ambColor`.
Same class as the file-select `fileselect-wash-clof-ambient-fix` (read the live ambient register,
not the block). Frame-verified: back-facing palm fronds go from black-crushed → mid-green.

## VERIFIED (structural, per handoff — no pixel oracle for sms-boot lighting)
- `[light] nlights=8 loads=8 do_light=1` (was `nlights=0 loads=0`).
- `[stage-light] LightGroup count=15`, `AmbGroup count=6`; L0 white at view-space (307,-809,-747).
- Frame dump `scratch/frames/boot_6121_amb.png`: palm fronds show shading VARIATION + correct
  mid-tone ambient (not flat full-bright, not all-black). ctest 28/28 (`-E platform_test`).

## Honest caveats / NOT done
- **All 15 Light Group positions are world-origin (0,0,0)** — the data is a colour palette at the
  origin; no runtime sun-direction setter on this path (grep `GXInitLightDir` → none stage-specific).
  So this is faithfully a **point light at the world origin** (radial shading), NOT a directional
  sun. If the final look is wrong, the real sun dir is applied elsewhere (JStage during cutscenes) —
  investigate THAT, do not fudge positions (NO BANDAIDS). Acceptable for now: shading varies.
- Top-half **SKY still black** — separate known gap (unrelated to lighting).
- Specular (GX_LIGHT2) / the gpLightManager unk54&&unk55 2nd-light gate / TLightShadow+Mario
  per-actor relighting are not modelled — only the stage diffuse+ambient. Fine for the static scene.
