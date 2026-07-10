# 2026-07-10 — Phase-tracker provider restored; title backdrop: indexed-mtx suspicion FALSIFIED, real cause narrowed to a behind-camera W-clip discard

## Part 1: sb_boot_capture_phase weak-symbol crash — provider restored

`sb_boot_capture_phase()` was declared `extern "C" ... __attribute__((weak))` at three
reference/sms callsites (`J3DDrawBuffer.cpp`, `JDRDrawBufObj.cpp`, `Map.cpp`) but its real
provider died with the deleted Path-B capture layer
(`native/render/sms_boot_j3d_capture.cpp`, `native/src/scene_drive.cpp`). Git-history trace
(`git log --all -S sb_boot_capture_phase` in both the superproject and the reference/sms
submodule):

- `reference/sms@2f288d9c` (2026-06-30) first stamped the phase in `TMarDirector::direct`
  via `sb_boot_capture_set_phase(1..6)` — 1=unk40, 2=unk38, 3=unk3C, 4=mPerformListGX,
  5=mPerformListSilhouette, 6=mPerformListGXPost — gated behind a `sb_capture_now` flag
  (`sb_own_gxlist() && sb_boot_capture_begin_scene()`), the deleted Path-B capture-buffer
  once-per-present lock.
- superproject `e9d3485` (same day) added the getter `sb_boot_capture_phase()` reading
  `g_capture_phase`, defined in the now-deleted `native/render/sms_boot_j3d_capture.cpp`.
- The one-runtime consolidation (2026-07-07) deleted both provider files but left the
  reference/sms callsites (declared weak, so link still succeeds) AND left
  `sms-boot/runtime/sdk_stubs.cpp` with a **wrong-signature** no-op
  `void sb_boot_capture_set_phase(int) {}` that silently discarded the phase, plus orphan
  no-op stubs for the retired `sb_own_gxlist`/`sb_boot_capture_begin_scene`/
  `sb_boot_capture_end_scene` gate. `sb_boot_capture_phase()` (the getter) had NO definition
  anywhere in sms-boot → undefined weak symbol → null function pointer → the two call
  sites without JDRDrawBufObj.cpp's `&fn ? fn() : -1` guard (`J3DDrawBuffer.cpp`,
  `Map.cpp`) called through null and crashed instantly under SB_DBHEAD_DBG/SB_MAPXLU_DBG.

### Fix (root cause, not a guard-only patch)

1. **New real provider**: `sms-boot/runtime/phase_track.cpp` — a plain
   `static int g_sb_capture_phase` with `sb_boot_capture_set_phase`/`sb_boot_capture_phase`
   get/set. Added to `sms-boot/CMakeLists.txt`'s explicit source list (reconfigured).
2. **Removed the vestigial capture-lock gate** (`reference/sms` `MarDirectorDirect.cpp`):
   the `sb_own_gxlist()`/`sb_boot_capture_begin_scene()`/`sb_boot_capture_end_scene()` calls
   belonged to the deleted Path-B capture buffer and had NO surviving purpose — worse, their
   sms-boot stubs had signature mismatches (`sb_boot_capture_begin_scene(int)` vs the
   declared `int sb_boot_capture_begin_scene()`; `void sb_own_gxlist()` vs declared
   `int sb_own_gxlist()`) that read garbage return values through extern-"C" linkage (UB,
   not a crash, but not correct either). `sb_boot_capture_set_phase(1..6)` is now called
   **unconditionally** at each perform-list dispatch site — it's a plain global write with
   no precondition; gating it behind a dead capture-lock was itself the bug once that lock's
   provider was gone. The 3 dead stub bodies (begin/end_scene, own_gxlist) removed from
   `sms-boot/runtime/sdk_stubs.cpp` along with their now-unused `extern "C"` declarations in
   `MarDirectorDirect.cpp`.
3. **Guarded the 2 unguarded callsites anyway** (defense for any other link unit, e.g. tests,
   that pulls these TUs without `phase_track.cpp`): `J3DDrawBuffer.cpp`'s SB_DBHEAD_MAT and
   SB_DBHEAD lines, and `Map.cpp`'s SB_MAPXLU_DBG line, now match JDRDrawBufObj.cpp's
   `&sb_boot_capture_phase ? sb_boot_capture_phase() : -1` pattern.

### Verification

`SB_HEADLESS=1 SB_STAGE=15 SB_DBHEAD_DBG=1` for 70 s (`scratch/logs/wf_dbhead_ok.log`):
1432 `[dbhead]` lines, no crash, phases take only the 3 values that actually occur at the
title (1, 4, 6 — no silhouette/list-3/list-2 traffic there), e.g.:

```
[dbhead] phase=1 buf=0x7ff12901eeb0 packets=6 name="DrawBuf Sky Xlu"
[dbhead] phase=4 buf=0x7ff1290201a0 packets=14 name="DrawBuf Mirror Opa"
[dbhead] phase=6 buf=0x7ff12901f210 packets=2 name="DrawBuf MapXlu"
```

Matches the documented phase semantics (MapXlu's ph1+ph6 routing vs its siblings' ph1+ph4,
the exact divergence this instrument was originally built to see, per the 2026-06-30
fileselect-overbright arc).

## Part 2: title backdrop black — indexed-matrix hypothesis FALSIFIED; real cause narrowed

Prior sessions (debug_journal/2026-07-07_title_backdrop_and_indexed_mtx.md,
2026-07-09_aurora_repoint_regressions_and_null_texmap.md, 2026-07-10_title_fidelity.md)
already landed the GXLoadPosMtxIndx/GXLoadNrmMtxIndx3x3 CP LOAD_INDX implementation and
concluded (continuation 9) "matrices/projection/cull/blend all verified correct" for the
visible logo-letter draws, with the backdrop's cause narrowed to "extreme dome verts land
mostly offscreen" — an open question at session start. This session's task named the
indexed matrices as the PRIME suspect; fresh instrumentation falsifies that framing and
sharpens the real defect.

### Falsification: indexed pos/nrm matrices are NOT garbage

`SB_DRAW_DUMP=3000` and a new `SB_NDC_PROBE`/`SB_NDC_MARK` run (existing aurora
instrument, `extern/aurora/lib/gx/command_processor.cpp`) dumped the resolved posmtx for
both `DrawBuf Sky Xlu` and `DrawBuf MapOpa` at the title. Every sample is a well-formed,
non-degenerate orthonormal rotation + translation (checked the MapOpa rotation block's
determinant by hand: ≈ +1.00007 — a proper rotation, not a mirror/reflection or scale-zero
collapse):

```
posmtx=[0.29 0.00 0.96 305.42 | -0.50 0.85 0.15 -830.92 | -0.81 -0.52 0.25 -723.24] mark='DrawBuf MapOpa'
```

The WGSL the shader compiler emits for this pipeline also consumes the matrix correctly:

```wgsl
let in_pnmtxidx = ubuf.current_pnmtx;
let mv_pos = vec4f(in_pos, 1.0) * ubuf.postex_mtx[in_pnmtxidx];
out.pos = vec4f(mv_pos, 1.0) * ubuf.proj;
```

**Verdict: GXLoadPosMtxIndx/GXLoadNrmMtxIndx3x3 and their CP LOAD_INDX consumer are not the
cause of the black backdrop.** That subsystem is correctly implemented, as the 2026-07-09
session already concluded — this session re-confirms it with new, independently-quoted
values rather than deferring to the old note.

### New decisive finding: MapOpa's perspective draws are 100% behind the camera (clip.w ≤ 0)

Extended the existing `SB_NDC_PROBE` CPU-side vertex-shader replication (already built for
exactly this class of question — "off-screen due to a transform bug" vs "genuinely
missing") with a companion dump (`[ndc-probe-behind]`, gated `SB_NDC_PROBE`, no new env var)
that prints the resolved pos-matrix and camera-space position for vertices whose `clip.w`
comes out ≤ 0 (GC/GPU convention: behind-camera, discarded pre-rasterization) — previously
these vertices were silently `continue`d past the print statement, so only the
"wneg=vtxCount" summary count was visible, never the actual numbers.

Result for **every** `DrawBuf MapOpa` perspective (`proj=P`) draw sampled (10+ consecutive
draws, `wneg=4` out of `vtxCount=4` on each):

```
[ndc-probe-behind]  v0 idx=0 pos=(-3948.9,-260.0,1811.1) mtx=0
  M=[0.290 0.000 0.957 305.422 | -0.479 0.866 0.145 -810.631 | -0.828 -0.501 0.251 -745.904]
  mv=(892.2,1120.4,3110.0) clipW=-3110.021 mark='DrawBuf MapOpa'
```

`clipW` is computed as `-mv.z` (confirmed algebraically: `mv.z=3110.0` → `clipW=-3110.021`,
i.e. the projection's W row is the standard `[0,0,-1,0]`). `mv.z` for every one of these
vertices is **positive** (+2500..+3150 range across all samples) — under the GC forward
`-Z` view-space convention, a positive camera-space Z means the point is **behind the eye**,
so `clipW<0` and the GPU/CPU-replica both correctly discard it. This is not a rounding
error or a marginal frustum-edge case: it is exact and 100% consistent across every sampled
draw.

### Classification (per the 3 buckets asked for)

- **Not** "matrices wrong/degenerate (all-zero → collapse)" — ruled out above, the matrix is
  a proper orthonormal rotation + sane translation.
- **Not** "TEV output black" — these vertices never reach the fragment stage; they're
  discarded at the vertex/clip stage before any TEV evaluation.
- **Closest to** "depth/blend discard", but more precisely: a **near/far-agnostic
  behind-camera (w≤0) clip discard**, caused by the map geometry's camera-space Z landing on
  the wrong side of the eye plane. Since the rotation matrix itself is well-formed (det≈+1,
  not mirrored), the likely culprit is upstream of the per-shape indexed-matrix mechanism —
  either (a) the camera/view matrix feeding this particular draw pass is oriented 180° from
  what the map geometry expects (e.g. a stale/wrong camera snapshot bound at the ENTRY
  perform-list phase — recall MapOpa flushes at BOTH phase=1 and phase=4/6, per Part 1's
  restored diagnostic, and phase=1 is the ENTRY pass, which may run before the frame's real
  camera has been recomputed), or (b) a genuine forward-axis sign convention mismatch
  between whatever computes `mDrawMatrices`/`mNormMatrices` for this shape and the
  projection's assumed `-Z`-forward convention.

**This is the "larger" case** — pinning down WHICH camera computation feeds this specific
draw, and why its Z convention disagrees with the (verified-correct) projection matrix,
needs proper RE of the TCamera/J3D view-matrix-build chain (or a Dolphin-oracle
per-draw view-matrix capture, same class of tool as `scratch/oracle/oracle_draws.log` from
the 2026-07-07 arc) rather than a guessed sign flip. Per the no-bandaids rule, NOT
attempting a blind negate-Z patch here — that would be exactly the "magic constant to make
output line up" pattern this project bans. Named precisely and stopped, per instruction.

### Frame-dump verdict

Backdrop is still black — this session did not land a fix for Part 2, only sharpened the
diagnosis (from "indexed matrices" prime-suspect → falsified; from "mostly offscreen" →
"100% behind-camera on the sampled perspective draws", a much narrower, more actionable next
step). Next session: capture the SAME draw's camera/view matrix from the Dolphin oracle
(`extern/dolphin`, `SB_ORACLE_DRAWLOG`-style hook) and diff against `M` above to see whether
the oracle's map draws also show positive camera-space Z (meaning the CONVENTION assumption
here is backwards) or negative (meaning native's camera/view computation for this pass is
genuinely wrong).

## New diagnostics landed (permanent, env-gated)

- `sms-boot/runtime/phase_track.cpp` — real `sb_boot_capture_set_phase`/
  `sb_boot_capture_phase` provider (Part 1).
- `extern/aurora/lib/gx/command_processor.cpp` `[ndc-probe-behind]` — companion to the
  existing `SB_NDC_PROBE`/`SB_NDC_MARK`, prints resolved pos-matrix + camera-space position
  for the first 6 perspective-draw vertices whose `clip.w≤0` (previously silently skipped).

## Part 3 (2026-07-10, continuation): "aurora MTX sign-convention regression" hypothesis FALSIFIED with a permanent unit test; defect re-confirmed and re-quantified, root cause still open

This session's task was framed as "aurora's view/projection matrix functions have a
sign-convention error vs the GC SDK" (prime suspect: `C_MTXLookAt`/`C_MTXPerspective`/
`C_MTXFrustum`/`C_MTXOrtho`, citing the aurora lineage's past LOAD_INDX/GXGetTexObjAll
regressions as precedent). Verified this framing is **wrong** before touching any code:

- Diffed `extern/aurora/lib/dolphin/mtx/{mtx.c,mtx44.c}` against
  `reference/sms/src/dolphin/mtx/{mtx.c,mtx44.c}` (the vendored Nintendo SDK source, the
  authoritative ground truth — reference/sms's own `dolphin/mtx/*.c` is excluded from the
  native build in `sms-boot/CMakeLists.txt` line ~30, so **every** game call to
  `C_MTXLookAt`/`C_MTXPerspective`/`C_MTXConcat` already resolves to aurora's
  `aurora::mtx` target, not a copy — there is only one implementation in the running
  binary). `C_MTXLookAt`, `C_MTXPerspective`, and `C_MTXConcat` are **byte-identical**
  between the two trees (same formulas, same operand order, right down to the
  `-((camPos->z*vRight.z)+...)` parenthesization). `GXSetProjection`/`GXProject` in
  `extern/aurora/lib/dolphin/gx/GXTransform.cpp` also match the known GC SDK layout
  (w-row `[0,0,-1,0]`, `wc = 1/-peye.z`).
- Added `sms-boot/runtime/tests/mtx_lookat_test.cpp` (ctest `platform_mtx_lookat_test`),
  linked directly against `aurora::mtx` (the real shipped implementation, not a hand
  copy) — 4 camera setups (origin-relative, offset-along-view-axis, looking down +X, and
  the projection w-row) all assert the GC convention (forward = **-Z** in view space,
  `clip.w = -mv.z`, a point in front of the camera has negative view-space Z / positive
  clip.w). **All pass.** This is a permanent regression guard: if a future aurora bump
  ever does regress this sign convention (the stated failure mode for LOAD_INDX/
  GXGetTexObjAll in this project's history), this test goes red immediately instead of
  requiring a fresh multi-hour RE pass to rediscover it.
- Re-ran `SB_NDC_PROBE=1 SB_NDC_MARK=MapOpa` fresh (this session, HEAD =
  `1555671`) and got the **same matrix and the same mv.z range** already recorded in Part
  2 above (`M=[0.290 0.000 0.957 305.422 | -0.479 0.866 0.145 -810.631 | -0.828 -0.501
  0.251 -745.904]`, `mv.z` in the `2500..3150` range) — the defect is real, reproducible,
  and unchanged since Part 2, not stale. But the framing in Part 2 ("every DrawBuf
  MapOpa perspective draw sampled") overstates it: quantifying this run's 400 sampled
  `MapOpa` draws by `wneg` (vertices discarded behind-camera out of `vtxCount`), **243
  draws have `wneg=0`** (fully in front, correct) and **82 draws have `wneg=4`** (100%
  behind camera, matching Part 2's quoted matrix exactly); the remainder are partial
  (dome/enclosing-mesh-style mixed visibility, expected for non-planar geometry). So: one
  specific recurring object/shape-group bound through `mtx=0` with this exact
  view-matrix is textbook-consistently 180°-wrong; most other `MapOpa` draws in the same
  frame are fine. Same shape holds for `DrawBuf Sky Xlu` (306/400 sampled draws
  `wneg=0`, the rest partial-visibility consistent with an enclosing sky-dome mesh, not a
  systemic flip) — so the black backdrop is not "the whole 3D world is behind the
  camera", it is this narrower, already-quantified subset.

**Conclusion: do not patch the MTX library — there is nothing wrong there, in isolation
or now under a permanent test.** The defect is exactly where Part 2 already narrowed it:
upstream, in which camera/view matrix gets concatenated into the `M` bound for this
specific `MapOpa` draw's `mtx=0` slot. Per the no-bandaids rule, this session does not
attempt a guessed 180°/negate-Z patch on top of a falsified hypothesis. Named per Part
2's still-standing next step: RE which `TCamera`/J3D view-matrix-build call produces this
exact `M` (phase=1 entry-pass stale camera vs. phase=4/6 recomputed camera, per Part 1's
restored phase instrument, is the leading candidate — it directly explains why some
`MapOpa` draws in the *same frame* are fine and others aren't) — a Dolphin-oracle
per-draw camera capture (`scratch/oracle/oracle_draws.log`-class tool) diffed against
this `M` would confirm which side (native's camera build vs. this convention assumption)
is wrong, faster than guessing.

### Frame-dump verdict (this session)

`SB_DUMP_FRAME_AFTER=600` lands mid-way through the SMS logo/wipe intro (2D `J2DScreen`
splash over a black background — expected, that background is black on real hardware
too); this is not evidence about the 3D backdrop either way. A later dump
(`SB_DUMP_FRAME_AFTER=3000`) is needed to sample past the intro into the live title
scene; see `scratch/frames_title/probe_check_late.png` for that capture from this
session.
