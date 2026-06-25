# GX → raylib (rlgl) switch — design & phasing

**Directive (user, 2026-06-25):** the GX API seam (`native/platform/gx_*.cpp`) must be
reimplemented so the game's **GX calls translate directly into raylib/rlgl calls**, replacing
the bespoke capture→Vulkan(nvk)/ngx rasterizer. GX immediate mode maps naturally onto rlgl
immediate mode. ("I know the scope but the switch needs to happen.")

## Foundation — PROVEN (P0, 2026-06-25)
`scratch/raylib_smoke/` (FetchContent raylib 5.5): a **hidden GLFW window on `DISPLAY=:0`**
gives a working headless GL context; `LoadRenderTexture` offscreen + `rlBegin/rlVertex/rlEnd` +
`LoadImageFromTexture` readback all work (green tri at (40,200,60) over clear (10,10,40)).
Xvfb is the fallback if `:0` is ever absent. So raylib can be the GL layer with NO display window.

## Current architecture (what we replace)
- `native/platform/gx_imm_impl.cpp` — GXBegin/Position/Color/TexCoord/End → captures native
  `SbImmVtx` batches, xformed by captured proj+posmtx, handed to `sms_boot_present.cpp`.
- `native/platform/gx_impl.cpp` — state setters (GXSetTev*, GXLoadPosMtxImm, GXSetChanCtrl,
  GXInit/LoadTexObj, GXSetBlendMode/ZMode, projection/viewport…) → mutate `gx_state.h` context.
- `native/platform/gx_fb_impl.cpp` — GXCopyDisp/CopyTex/DrawSphere/DrawCube/Peek/Poke/clear.
- J3D scene geometry comes through `scene_drive.cpp` → object-model walk → `nvk`/`ngx` render
  (NOT the imm path). The 2D/HUD content comes through the imm path.
- Renderer: `native/render/nvk.cpp` (offscreen Vulkan rasterizer) + `runtime/render`+`runtime/ngx`
  (TEV combiner shaders in GLSL 450 via `sb_tev_gen_fragment`, push-constants + descriptor sets).

## Target mapping (GX → rlgl)
| GX | rlgl |
|---|---|
| GXInit | create hidden window + `rlglInit` + offscreen RenderTexture (EFB size) |
| GXLoadPosMtxImm / GXSetCurrentMtx | `rlMatrixMode(RL_MODELVIEW)` + load 3x4→4x4 |
| GXSetProjection | `rlMatrixMode(RL_PROJECTION)` + load (persp/ortho) |
| GXSetViewport / GXSetScissor | `rlViewport` / `rlScissor` |
| GXBegin(prim,fmt,n) | remember prim; begin accumulating |
| GXPosition3f32 | `rlVertex3f` (strips/fans/quads → triangulate to RL_TRIANGLES) |
| GXColor4u8 / GXColor1u32 | `rlColor4ub` |
| GXTexCoord2f32 | `rlTexCoord2f` |
| GXEnd | flush prim verts into the active rlgl batch |
| GXInitTexObj(+CI/LOD/Tlut) | decode GC fmt → RGBA (reuse `tex_decode`) → `rlLoadTexture` (cache by image ptr) |
| GXLoadTexObj | `rlSetTexture(id)` (flush batch on change) |
| GXSetBlendMode | `rlSetBlendMode` / custom `rlSetBlendFactors` (GX factors→GL) |
| GXSetZMode | `rlEnableDepthTest`/`rlDisableDepthTest` + depth func/mask |
| GXSetTev* (combiner) | bind a **GLSL-330 TEV shader** generated from the stage state; flush batch on TEV-state change |
| GXSetChanCtrl/Mat/AmbColor + lights | per-vertex light into rlColor (CPU) OR shader uniforms |
| GXSetFog | shader fog block (the missing stage today) |
| GXCopyDisp | resolve offscreen → readback (dump) / blit to window (live present) |
| GXDrawSphere/Cube | triangulate procedurally into rlgl (already done in capture form) |

## The hard parts (sequence them)
1. **Headless context lifecycle** — GXInit may be called before a display is guaranteed; init
   raylib lazily on the first real draw/copy, guard double-init, Xvfb fallback.
2. **TEV combiner on rlgl** — rlgl batches under ONE shader. On a GX TEV-state change:
   `rlDrawRenderBatchActive()` (flush) then `rlSetShader(tevProgram, locs)`. Generate GLSL **330**
   (in-vars, `uniform` instead of push_constant/descriptor-set) from the existing TEV state — fork
   `sb_tev_gen_fragment` into a GL-330 emitter (same combiner logic, different preamble/uniforms).
   Phase: start with rlgl's DEFAULT shader (texture×vertexColor = GX_MODULATE, covers most mats),
   add per-material TEV shaders after first-light.
3. **J3D scene geometry** — today via object-model walk, not GX imm. Either (a) route J3D shape
   draws through `GXCallDisplayList` (decode the GC DL → rlgl verts with the bound pos/tex mtx), or
   (b) keep `scene_drive`'s object-model walk but emit rlgl verts instead of nvk batches. (b) is
   less faithful to "GX→raylib" but far less work; (a) is the true switch. Decide at P3.

## Phasing (each phase = a verifiable milestone via the frame dump)
- **P1** Add raylib to the build (FetchContent in `native/CMakeLists.txt`); `gx_raylib.cpp`:
  GXInit→context+offscreen; GXCopyDisp→readback into the existing present/dump. All draws stubbed.
  Verify: a dump frame appears at the captured GXSetCopyClear color. Behind `SB_RAYLIB=1`.
- **P2** Immediate-mode 2D: GXBegin/Position/Color/TexCoord/End + matrices + textures (default
  modulate shader). Verify: file-select 2D panes/fader/glyphs render through raylib.
- **P3** J3D scene (chosen route) → rlgl verts. Verify: plaza/beach geometry renders.
- **P4** Per-material TEV shaders (GLSL-330 from TEV state) + PE alpha test + blend/zmode fidelity.
- **P5** lighting / fog / indirect / texgen fidelity. Then make `SB_RAYLIB` the default and retire
  `nvk` + the `ngx` Vulkan path.

## Verification
Same value-first discipline: per-frame dump (`SB_FRAME_DUMP` → `scratch/frames/boot_NNNN.ppm`,
now correctly named after the off-by-one fix, commit 485e438) + region brightness + the parity
dump. Never eyeball-only. Build a raylib-vs-(known-good) compare per phase.

## Notes / gotchas captured this session
- The frame-240 "overbright white" is the **2D IMM fade overlay**, NOT 3D scene textures
  (SKIP_IMM drops 254.5→180.6; NO_DRIVE_SCENE stays 255). The handoff's #1 root-cause was a
  stale-file artifact. The 3D scene under it is ~180 (plaza) / normal (settled beach sky+sea ok,
  sand bright).
- `fileselect_gx_oracle.png` is NOT clean pure-Dolphin — it carries our engine STATE bugs
  (visible with-FLUDD Mario copy, white shadow). Don't diff against it for Mario fidelity.
