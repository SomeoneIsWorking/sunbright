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
- **P1 ✅ DONE (commits 27fecd9 + 760f927/3f885d5)** raylib in the build; `gx_raylib.cpp` context+
  offscreen+readback; SB_RAYLIB present branch dumps the GX copy-clear colour. The two integration
  blockers (radeonsi LLVM SIGSEGV; raylib/Mesa JKR SolidHeap OOM) are fixed by the **foreign-thread
  alloc isolation**: a thread-local game-thread flag (JKRHeap.cpp) routes any plain `new` from a
  non-game thread (Mesa/LLVM driver workers) to host malloc, and the raylib entry points raise the
  host-alloc gate so synchronous driver allocs on the present (game) thread also bypass the JKR heap.
  Default GL3.3 now runs SB_RAYLIB with no crash / no OOM (zink workaround retired).
- **P2 ✅ DONE (immediate-mode replay)** `draw_tev()` in gx_raylib.cpp replays the captured combined
  `verts`/`batches` (scene + 2D imm) as rlBegin/rlVertex3f/rlColor4ub/rlTexCoord2f, identity matrices,
  per-batch texture (rlLoadTexture cache) + GX→GL blend (`gx_blend_factor`, mirrors nvk) + depth.
  Verified: the Delfino plaza renders through rlgl — buildings/umbrella/water/Mario at the SAME
  positions/orientation/winding as the nvk reference (frame 240, scene-only A/B). **Two residuals,
  each owned by a later phase (NOT bandaids — the proper fix is named):**
  - **R1 → P3:** 3D near/side clipping is CPU-approximate (a clip-space `w>=eps` Sutherland-Hodgman
    in `emit_tri_clipped`) because rlgl IMMEDIATE mode carries only a 3-component position (and
    `rlNormal3f` normalises, so `w` can't ride the normal). This leaves a sky-dome starburst + a
    black foreground disc where the hardware near-clip nvk relies on would clip cleanly. **Proper
    fix:** a custom rlgl vertex buffer (rlLoadVertexArray/Buffer) with a 4-component position + a
    custom GLSL-330 VS `gl_Position = vec4(clip.xyz, clip.w)`, feeding TRUE clip-space xyzw so the
    GPU does native near/side clipping + perspective-correct interpolation — exactly nvk's contract.
  - **R2 → P4:** colours are darker/desaturated vs nvk because `draw_tev` uses rlgl's DEFAULT modulate
    shader (texture × vertexColor) and ignores each batch's captured TEV combiner (`fragGlsl`). The
    frame-240 white FADE overlay is invisible for the same reason: its raster alpha is 0 and its white
    output comes entirely from the TEV combiner (`ib0 c0env=0008fffa`). **Proper fix = P4.**
  - Diagnostics: `SB_RAYLIB_MAXBATCH=N` (draw first N batches), `SB_RAYLIB_NODEPTH=1` (painter's order).
- **P3** True clip-space scene via custom VBO + GLSL-330 VS (R1 above). Verify: plaza/beach geometry
  renders with no starburst/black-disc, matching nvk geometry.
- **P4** Per-material TEV shaders (GLSL-330 from TEV state, fork `sb_tev_gen_fragment` / `imm_tev_fragment`)
  + PE alpha test + blend/zmode fidelity (R2 above). Verify: scene + 2D fade colours match nvk.
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
