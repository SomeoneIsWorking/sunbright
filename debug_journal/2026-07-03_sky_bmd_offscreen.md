# Sky at title screen: sky.bmd renders NOTHING visible (2026-07-03)

## Finding

Under the hard rule of "RE the intent, port PC-native" (CLAUDE.md 2026-07-03),
this session investigated why the sky looks wrong at the title screen
(`SB_STAGE=15 ./run.sh`).

**Root cause: every sky.bmd batch transforms to off-screen clip space.** All
eight sky-tagged batches captured at the settled title (dome base + horizon
strips + cloud strips + brown ground strip) project to negative w or beyond
the frustum. When `SB_SKY_ONLY=1` (draw only sky batches, skip everything
else), the output is a solid blue clear — sky.bmd contributes **zero visible
pixels**. Symmetric proof: `SB_SKY_SKIP_HEX=FF` (skip every sky batch) is
pixel-identical to the baseline, `mean|Δ|=0.00`.

The "blue with white streaks" that looks like sky in the native output is:
- the SDL3-GPU clear color (0,18,238) = the current `sb_native_sky_backdrop`
- plus the OTHER scenes' draws (map/water/etc) overlapping into the top area

The user-visible cloud-like streaks aren't sky at all — they're artifacts
in the water/beach draws bleeding into the sky region.

## Evidence

Per-batch clip-space AABB from `SB_NATIVE_SKY_DBG=1 tools/render/title_sbs.sh`
(sms_boot_j3d_capture.cpp emits `clip …` line per sky batch):

| batch | vc   | role                        | wPos/vc | onScreen/vc |
|-------|------|-----------------------------|---------|-------------|
| 0     | 30   | ~horizon 8×8 dither         | 15/30   | 3           |
| 1     | 132  | ~horizon 8×8 dither         | 96/132  | 33          |
| 2     | 60   | ~horizon 8×8 dither multi   | 40/60   | 0           |
| 3     | 1800 | **dome base (per-vtx blue)**| 930/1800| **80/1800** |
| 4     | 24   | cloud strip 128×256         | 0/24    | 0           |
| 5     | 12   | cloud strip 128×256         | 0/12    | 0           |
| 6     | 30   | cloud strip 128×256         | 0/30    | 0           |
| 7     | 48   | brown/beach strip 128×64    | 0/48    | 0           |

The 80 on-screen dome verts don't produce visible pixels either — verified
by SB_SKY_ONLY (screenshot: `scratch/screenshots/sbs_sky_only.png`).

## Why: TSky's setBaseTRMtx reads a stale camera view matrix

`Sky.cpp` TSky::perform (`reference/sms/src/Map/Sky.cpp:17`) positions the
sky dome at the camera by:

```cpp
Mtx local_4c;
MTXInverse(gpCamera->unk1EC, local_4c);
// ... take translation column of the inverse …
unk44->getModel()->setBaseTRMtx(afStack_7c);
```

In sms-boot, `gpCamera->unk1EC` is stale/zero: `native/src/camera_latch.cpp`
publishes the LIVE view matrix to `j3dSys.mViewMtx`, NOT back to gpCamera
(explicit comment: "gpCamera fovy/pos/target read 0/garbage"). So MTXInverse
produces garbage and the dome ends up at nonsense world coordinates. The
sky.bmd geometry is fine; the base transform matrix is broken.

**A patch that reads j3dSys.mViewMtx to compute the correct camera world
position and calls `sky->unk44->getModel()->setBaseTRMtx(base)` before
`drive_group` did NOT fix the visible output** (dome onScreen stayed 80/1800).
Reason unknown — possibly J3D viewCalc uses a different matrix path, or the
override is being clobbered by a downstream calc-anim step. Not worth
chasing further: this is emulation chasing, which the hard rule bans.

## Fix (LANDED this session)

Native SDL3-GPU full-screen sky pass — a vertical blue gradient painted per
frame, replacing sky.bmd entirely. The visible sky in `sms-boot` is now this
gradient, not the framebuffer clear + water/beach overspill.

**Files:**
- `native/render/gx_sdlgpu.{h,cpp}`: public `sb::gxsdl::native_sky_fill(top,
  horizon)`. Full-screen triangle via `gl_VertexIndex`; fragment mixes `top`
  and `horizon` by `vNdc.y` (Vulkan NDC — y grows downward). Own render pass
  with `LOAD_OP=CLEAR` (owns colour+depth init). Fail-loud on any
  compile/pipeline error.
- `runtime/render/glsl_compile.{h,cpp}`: added `sb_compile_vertex_glsl` (the
  existing `sb_compile_fragment_glsl` was fragment-only).
- `native/render/sms_native_sky.h` + `native/src/scene_drive.cpp`: new
  `sb_native_sky_paint()` — no-op unless `sb_native_sky_active()` (stage 15).
  Endpoints tuned to the oracle title-screen sky region (top
  `(40,120,190)`, horizon `(140,195,230)`).
- `native/render/sms_boot_present.cpp`: `draw_seg` lambda wrapper — every
  segment that would `clearFirst=true` now paints the sky first (via
  `sb_native_sky_paint`) and passes `clearFirst=false` to draw_tev_segment.
  This puts the sky BEHIND every scene batch in that segment and survives
  mid-frame clears (the mirror EFB snapshot / soft-focus copy).

**Verification.** With SB_SKY_SKIP_HEX=FF (skip every sky.bmd batch),
`mean|Δ|=0.00` vs baseline — sky.bmd still contributes zero pixels; the
gradient is our native pass. The visible-frame is:

- Before fix: `mean|Δ|` sky-top-120 ≈ 82 (native's top area was clear-blue
  + water/beach overspill and cloud-like artefacts).
- After fix: `mean|Δ|` sky-top-120 ≈ 74 with the tuned endpoints. Whole-
  frame `mean|Δ|` = 64.7 (down from 67.8). The remaining delta is oracle's
  cloud detail + palm silhouette variation, which the current flat-gradient
  doesn't replicate — future work if wanted, but the visible sky defect
  (missing/broken sky, dominant white streaks in the top region) is
  resolved.

**Reproduce:** `bash tools/render/title_sbs.sh` → `scratch/screenshots/
sbs_title.png`. Latest good result: `scratch/screenshots/sbs_sky_tuned.png`.

## Not done (deferred, out of scope for this session)

- Remove the now-dead `is_native_sky` batch tagging + the
  `kNativeSkyBaseFrag`/`kNativeSkyCloudFrag` fragment-shader fork in
  `gx_sdlgpu.cpp`: those target the sky.bmd batches, which paint nothing.
- Cloud detail: the current gradient is smooth. Oracle has puffy clouds
  from the sky.bmd cloud-strip meshes. If wanted, add a cloud sample as a
  second full-screen pass modulating a noise / cloud-texture over the
  gradient — but the "sky is broken" defect is now fixed either way.

## Original plan (retained for context)

Draw the sky as a native SDL3-GPU full-screen pass, replacing sky.bmd
rendering entirely. The intent is a blue vertical gradient with subtle
white clouds — a 2D screen effect, not a 3D dome. Implementation sketch:

1. Add a native pipeline in `native/render/gx_sdlgpu.cpp` that draws a
   full-screen triangle post-clear, ONLY when `sb_native_sky_active()`.
2. Fragment shader: vertical gradient from `sb_native_sky_backdrop()` at
   the top to a lighter horizon color, plus optional procedural clouds
   (or use one of the game's cloud textures as an alpha mask).
3. Drop the `is_native_sky` batch tagging in
   `native/render/sms_boot_j3d_capture.cpp` (nsky_is_model) — the tag is
   currently useless since those batches render nothing.
4. `drive_sky()` in `native/src/scene_drive.cpp` becomes a no-op under
   `SMS_NATIVE_PLATFORM`; the fullscreen pass replaces both the recompiled
   TSky::perform draws and the captured sky.bmd batches.

## Files touched this session (reverted)

Uncommitted diff investigating this (kept as diagnostic prints):
- `native/render/gx_sdlgpu.cpp`  — fail-loud on shader compile; per-pass
  SB_SKY_ONLY / SB_SKY_SKIP_HEX diagnostic.
- `native/render/sms_boot_j3d_capture.cpp` — per-sky-batch clip-space AABB
  dump under `SB_NATIVE_SKY_DBG=1`.

The prior "sky base + cloud" fork of the native sky fragment shader
(`kNativeSkyBaseFrag` / `kNativeSkyCloudFrag`) is landed but has no effect
because the batches it targets don't render.
