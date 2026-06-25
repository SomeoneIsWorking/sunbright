# GX → SDL3 GPU switch — design & phasing

**Directive (user, 2026-06-25):** "SDL3 GPU is better." The GX seam's PC-native renderer runs on the
**SDL3 GPU API** (`SDL_gpu.h`), replacing the bespoke `nvk` Vulkan rasterizer. It is now the sole
renderer.

## Why SDL3 GPU (the reasons the pivot is right)
- **SPIR-V shaders.** SDL3 GPU's Vulkan backend consumes SPIR-V, so the EXISTING TEV pipeline
  (`sb_tev_gen_fragment` GLSL 450 + `runtime/render/glsl_compile.cpp` glslang→SPIR-V + the
  precompiled `tev_vert_spv.h`) plugs in almost as-is. The real TEV combiner comes essentially free.
- **Vulkan-style NDC.** SDL3 GPU clip space matches Vulkan (Y-down, depth [0,1]) — exactly what
  `NvkTevVertex` already carries. No Y-flip, no `2z-w` depth remap, no immediate-mode w-loss.
- **Explicit pipeline state objects** map 1:1 onto GX render state (blend / depth / cull).
- **Truly headless.** `SDL_VIDEODRIVER=offscreen` → no DISPLAY needed at all.

## Foundation — PROVEN (P0, 2026-06-25): `scratch/sdlgpu_smoke/`
Headless SDL3 GPU works: `SDL_Init(SDL_INIT_VIDEO)` (REQUIRED even windowless) + `SDL_CreateGPUDevice
(SPIRV)` → Vulkan backend, NO window/swapchain → render pass clear to an offscreen `R8G8B8A8` color
texture → copy-pass `SDL_DownloadFromGPUTexture` → `SDL_SubmitGPUCommandBufferAndAcquireFence` +
`WaitForGPUFences` → `MapGPUTransferBuffer` reads back EXACT pixels (40,200,60). Works both with
`SDL_VIDEODRIVER=offscreen` (no DISPLAY) and on `DISPLAY=:0`. SDL3 3.4.10 is a SYSTEM package
(`pkg-config sdl3`, `/usr/include/SDL3/SDL_gpu.h`, `libSDL3.so`) — vendor nothing.

## Reference: what we port FROM
`native/render/nvk.cpp` `renderTevFrame` is the exact reference (the SDL3 GPU backend reproduces it):
- Vertex layout = `NvkTevVertex`: loc0 pos `vec4` (clip xyzw), loc1 color0 `vec4`, loc2 color1 `vec4`,
  loc3..6 = uv[0,2,4,6] packed `vec4` pairs (8 texcoords). All `R32G32B32A32_SFLOAT`.
- Per-batch fragment shader = `sb_tev_gen_fragment(...)` (GLSL 450), cached by `NvkTevBatch::shaderKey`.
- Vertex shader = `native/render/shaders/tev.vert` (already SPIR-V `tev_vert_spv.h`; no samplers/UBO).
- Push constants = `NvkTevPush` {int kcolor[4][4]; int tevreg[4][4];} = 128 bytes, fragment stage.
- 8 combined-image-samplers (one per GX texmap), GX filter/wrap → `getSampler`.
- Per-batch pipeline state: cull NONE, depth test/func/write from the batch, GX blend → color blend.

## SDL3 GPU resource-binding REMAP (the one real shader change)
SDL3 GPU mandates fixed SPIR-V descriptor sets per the spec:
- Vertex stage: set0 = sampled textures, set1 = uniform buffers.
- Fragment stage: set2 = sampled textures, set3 = uniform buffers.
So the TEV **fragment** GLSL must declare `layout(set=2,binding=0..7) uniform sampler2D` and replace
`layout(push_constant)` with `layout(set=3,binding=0) uniform`. Push the 128-byte block via
`SDL_PushGPUFragmentUniformData(cmd, 0, &push, 128)`. `tev.vert` needs no change (no samplers/UBO;
plain location in/out + gl_Position) — reuse `tev_vert_spv.h` directly. Only the binding decorations
differ from the original generated GLSL.

## Phasing (each phase = a frame-dump-verifiable milestone, A/B vs nvk; SB_SDLGPU env selects it)
- **P0 ✅** headless device + offscreen color target + clear + readback (`scratch/sdlgpu_smoke`).
- **P1 ✅ (commit 5100605)** `gx_sdlgpu.cpp` in the build (system SDL3): device + offscreen color
  target + `frame_begin` clear + `readback`, wired into `sms_boot_present.cpp` behind `SB_SDLGPU`.
  Verified headless: frame 240 dumps the GX copy-clear (all-black).
- **P2 ✅ (commit b1e0a3d)** vertex buffer upload + one modulate pipeline (reused `tev_vert_spv` VS +
  minimal GLSL-450 texture*color FS, cull NONE) drawing all batches. NvkTevVertex feeds RAW (no
  Y-flip/depth-remap). One real fix: SDL3 GPU's clip→framebuffer Y is inverted vs raw Vulkan → flip
  rows on readback. Verified scene-only A/B vs nvk 71.6→16.6 (geometry matches; residual = no TEV/blend).
- **P3 ✅ (commit e00e1e9)** real per-material TEV: reuse each batch's `fragGlsl`, remap bindings
  (samplers set=2 individual, push_constant→set=3 uniform), glslang→SPIR-V, per-batch pipeline
  (blend/depth from GX state, cull NONE), NvkTevPush fragment uniform, 8 samplers. Verified vs nvk:
  scene-only **3.14**, FULL frame incl. 2D white fade **0.27** — pixel-identical.
- **P4 ✅ DONE** generated mip chains (`SDL_GenerateMipmapsForGPUTexture` + trilinear sampler →
  scene-only 0.07, pixel-identical to nvk); `SB_SDLGPU` made the DEFAULT; **nvk DELETED entirely**
  (geometry types extracted to `gx_geom.h`). The raylib path was retired earlier. SDL3 GPU is the
  sole renderer. Remaining fidelity (lighting/fog/indirect/texgen edge cases) is now driven by the
  parity ladder below, not A/B-vs-nvk (there is no nvk).

## Parity-focused renderer TDD (user directive, 2026-06-25)
The old render tests (nvk-based) were REMOVED: they only proved two of our own renderers agreed,
not that the output is CORRECT. The new suite is **faithfulness/parity TDD, small scope → large
scope**: render a KNOWN input through `sb::gxsdl` (the SDL3 GPU renderer) and assert the readback
pixels against **spec-computed ground truth** (GX semantics) — eventually the Dolphin-GX oracle for
larger scenes. Tests live in `native/render/tests/parity/*_test.cpp` (auto-globbed → ctest
`parity_<name>`, env `SDL_VIDEODRIVER=offscreen`). Ladder:
1. ✅ `raster_basic_test` — clear colour exact; one solid triangle's coverage + interpolated colour.
2. ✅ `texture_basic_test` — a known 2x2 texture sampled at known UVs → exact texels (filter/wrap).
3. ✅ `tev_combiner_test` — one TEV stage (REPLACE / MODULATE / KONST-MODULATE / ADD-with-clamp)
   through the SHIPPING `sb_tev_gen_fragment`, asserted PIXEL-EXACT (tol 0) vs hand-computed GX
   integer-combiner truth. Proves combiner math + push-constant (kcolor/tevreg) + raster + texture
   plumbing + clamp. Sensitivity verified: a 1-unit expectation error fails 784/784 px.
4. ✅ `blend_depth_test` — additive + alpha-over blend, two depth-test orderings (order-independent
   sort) via a passthrough shader. Sensitivity verified (disable z_test → far quad bleeds through).
5. ✅ `multibatch_scene_test` — four batches (REPLACE bg, MODULATE centre, alpha-over fg, a
   depth-REJECTED quad) composited into one frame, asserted per region vs hand-computed truth.
   Proves batch ordering + per-batch pipeline switching + cross-batch depth/blend.
   PLUS two coverage-deepening tests off the same generator:
   - ✅ `tev_advanced_test` — multi-stage TEV (stage1 reads stage0's PREV), the SCALE shift, and the
     PE-block ALPHA TEST discard (pass + discard). All pixel-exact.
   - ✅ `texture_filter_test` — sampler WRAP modes (REPEAT/MIRROR/CLAMP) exact + bilinear MAG (centre
     is a genuine blend, endpoints clamp). Sensitivity verified vs NEAREST.
6. **OPEN (capstone): full-frame vs the Dolphin-GX oracle.** The remaining rung. It needs a LIVE
   two-process A/B (the same plaza frame rendered by sms-boot SB_SDLGPU and by a Dolphin-GX reference)
   — there is no committed golden frame (a rendered SMS frame is copyrighted game imagery; never
   commit one). The hard part is cross-engine determinism: sms-boot runs the engine PC-native while
   the Dolphin-GX oracle runs under Dolphin's JIT, so reaching a frame-exact identical scene needs a
   sync point. Until that harness exists, the spec-truth unit rungs above (1-5 + advanced/filter) are
   the falsifiable coverage; the per-frame `SB_FRAME_DUMP` + `abppm.py` path is a manual regression
   check, NOT an automated correctness oracle.

## Verification (unchanged discipline)
Per-frame dump (`SB_FRAME_DUMP` → `scratch/frames/boot_NNNN.ppm`) + `scratch/frames/abppm.py`
(overall + 4x4-region mean-abs-delta). This is a manual regression aid (compare two of YOUR OWN runs
for drift); it is NOT a correctness oracle — correctness comes from the spec-truth parity ladder
above, and (rung 6, open) a live Dolphin-GX A/B. Never eyeball-only.

## Geometry contract (the rule that makes the scene match)
Feed **4-component clip-space** and use **cull NONE** (nvk used `VK_CULL_MODE_NONE`; SDL3 GPU sets
`SDL_GPU_CULLMODE_NONE`). The sky sphere is drawn from its INWARD faces, so any back-face culling
combined with the Y-down clip (which reverses winding) drops it — explicit cull-none avoids the whole
class. SDL3 GPU's Vulkan NDC matches `NvkTevVertex` directly, so there's no clip-space remap.

## Status
- **P0–P4 ✅ DONE.** SB_SDLGPU is the sole renderer (nvk + raylib retired/deleted). It renders the
  plaza headless (Vulkan, no DISPLAY), with TEV combiner + per-batch blend/depth + mip chains; the 2D
  white fade renders correctly. Scene parity reached 0.07 mean-abs-delta vs the old nvk reference
  before nvk was deleted.
- **Parity TDD:** ladder rungs 1-5 + `tev_advanced_test` + `texture_filter_test` are GREEN (7 ctest
  `parity_*` targets, all spec-truth, pixel-exact where the GX math is integer). Rung 6 (live
  Dolphin-GX full-frame oracle) is the open capstone — see the ladder above.

### Repro (headless, no DISPLAY)
```
cmake --build build-native --target sms-boot -j$(nproc)
rm -f scratch/frames/boot_0240.ppm; pkill -9 -x sms-boot; sleep 1
timeout -s KILL 70 setarch -R env SDL_VIDEODRIVER=offscreen SUNBRIGHT_DISC=scratch/disc/sms.iso \
  SB_SDLGPU=1 SB_THP_FAST=1 SB_TURBO=1 SB_HOST_ALLOC_CAP_MB=3072 SB_FRAME_DUMP=1 \
  SB_FRAME_DUMP_START=240 SB_FRAME_DUMP_MAX=1 ./build-native/sms-boot
# boot_0240.ppm = the SDL3 GPU frame. Drop SB_SDLGPU for the nvk reference. SB_SKIP_IMM=1 = scene-only.
# A/B: python3 scratch/frames/abppm.py <a.ppm> <b.ppm>   (overall + 4x4-region mean-abs-delta)
```
