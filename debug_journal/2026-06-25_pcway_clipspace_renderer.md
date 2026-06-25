# Native engine: render the 3D scene the PC way — clip-space w + GPU clipping (2026-06-25)

## Symptom (two proven bugs, prior handoff)
1. **Floor "abnormally stretched tiling"** — the Delfino pavement texture warped/stretched
   with distance (affine, not perspective-correct).
2. **Near-plane "slab"** — a triangle smeared across the screen edge near the camera.

## Root cause (named)
Both came from the capture renderer doing the **perspective divide AND clipping on the CPU**,
then handing the GPU pre-divided NDC with `gl_Position = vec4(inPos, 1.0)` (`tev.vert`):
- **w == 1** ⇒ the GPU interpolates all varyings (UV, colour) SCREEN-LINEARLY (affine), not
  perspective-correct ⇒ the floor texture warp.
- The hand-rolled near+side clipper (`sb_tri_clip.h`) clipped in NDC space, perspective-
  incorrect by its own comment, and could smear a triangle to the screen edge ⇒ the slab.

## Fix (own the path — render the PC way, drop GX for 3D, per the settled direction)
Emit **clip-space xyzw** per vertex and let the GPU do the divide → perspective-correct
interpolation + hardware near/side/guard-band clipping for free. No CPU divide, no CPU clipper.
- `gx_imm_xform.h`: new pure `imm_project_eye_clip(eye) -> {x,y,z,w}` built so
  `(x/w, y/w, z/w)` == the old `imm_project_eye(eye)` NDC EXACTLY (perspective: `w=-ez`,
  viewport-offset terms `2*vp0/vp2`, `2*vp1/vp3` folded into x,y; ortho: `w=1`, NDC==clip).
  On-screen position + depth are bit-for-bit unchanged; only the wrong interpolation/clipping
  is removed.
- `nvk.h`: `NvkTevVertex` gains `float w = 1.0f` (clip w; 2D/imm content keeps w=1 → unchanged).
- `nvk.cpp`: the TEV position vertex attribute widened to `R32G32B32A32_SFLOAT` (vec4 xyzw),
  both pipeline sites (`renderTevTriangles`, `renderTevFrame`).
- `shaders/tev.vert`: `in vec4 inPos`; `gl_Position = inPos` (GPU divides).
- `sms_boot_j3d_capture.cpp`: J3D shapes AND the GXDrawSphere sky dome now emit clip-space
  verts straight through (no `sb_clip_emit_tri`); the sky backdrop-Z pin moved to clip space
  (`clip_z = min(clip_z, kBackdropZ * w)`). The CPU clipper include is gone.

## Verified
- **Unit (the gate):** `native/render/tests/clipproj_test.cpp` — 27/27. Asserts clip/w == old
  NDC for perspective (with & without viewport offset), orthographic (w==1), and that w>0 in
  front of the camera / w<0 behind (so the GPU near-clips behind-camera verts instead of the
  CPU 1/-ez explosion). `ctest -R render_clipproj_test`.
- **Live Delfino frame** (`scratch/frames/clip_0260.png`, settled gameplay): the pavement now
  tiles in correct perspective — **floor smear GONE**; no near-plane slab. Buildings, sea,
  parasols, awning stall, and Mario (FLUDD over shoulders) all render cleanly. 41 frames
  presented, no NaN/segv/draw-buffer cycle. `scene_verts` rose ~22k→133k as expected: the CPU
  clipper used to drop off-screen/behind triangles; now all emit and the GPU clips.

## Notes / residual
- The `[bbox]`/`[batchdbg]`/sky-sphere debug stats now print clip-space (not NDC) for the 3D
  path — gated diagnostics only, not load-bearing; divide by `v.w` if exact NDC is wanted.
- `sb_tri_clip.h` is now dead (no users); left on disk documenting the retired approach.
- The pre-existing guest `SolidHeap OUT OF MEMORY` log lines are unrelated (guest JKR heap
  pressure on the THP fast path), not caused by the host-side vertex-count increase.
- Mario's skeleton is still unposed in sms-boot (separate, per the prior handoff: drive
  `perform(0x1)`/`calcAnim`); the body reads recognizable but unanimated.
