# 2026-07-16 — File-select camera pan/settle FIXED (matan octant + load-cam name); shadow view-matrix fix

User report: after PRESS START the camera pan was wrong and settled differently from Dolphin
(couldn't get a clean 1:1 view — OPTIONS sign pushed off-screen), and Mario's shadow was missing.

## ✅ CAMERA — two independent bugs, both fixed (reference/sms 59e43e6f)

### Bug 1: `matan` octant-offset error → mirrored camera yaw (the big one)
`MarioUtil/MathUtil.cpp::matan` (retail @0x8022ae08) is an octant-reduced atan2. A prior
"OOB-safety" rewrite emitted the WRONG octant offset in two diagonal branches — e.g. for
(param_1<0, param_2≥0, |param_1|≥param_2) it returned `atan(p2/|p1|) + 0x8000` where retail
returns `0x8000 - atan(p2/|p1|)` — which REFLECTS the returned angle.

Chain to the symptom: `CLBRevisionLookatByAngleX` (called every frame from `calcFinalPosAndAt_`)
round-trips the look target through `CLBCrossToPolar` → clamp pitch → `CLBPolarToCross`, using
`matan` for the yaw. The reflected yaw mirrored the target's X around the eye:
`ctrlOptionCamera_` produced target=(1148.47,…) but `C_MTXLookAt` received (1041.53,…) —
eye.x+53.5 became eye.x−53.5, i.e. the camera looked the wrong way and the 3D scene shifted
so OPTIONS fell off-screen. Instrumented with SB_LOG=camlook (final lookat inputs).

Fix: rewrote `matan` as a faithful transcription of the retail decompile. Retail is OOB-safe by
construction (every branch divides by the larger magnitude so the table index ≤ 1024), so the
rewrite is both correct AND crash-free — the earlier hack was unnecessary. Verified 0 error vs
`atan2` over 20k random inputs + full octant sweep. Close-test:
`sms-boot/shims/tests/matan_octant_test.cpp` (RED on the old branch offsets, GREEN now).

### Bug 2: wrong load-camera name → pan aimed at the origin
`TCameraOption` ctor resolves the load/settle camera from the option-scene map-tool. The port
searched for `"ロードカメラ"` (does not exist in the tool table); retail (ctor @0x80032130) reads
SDA2 string @0x80375ac8 = `"左サイドカメラ"` (left-side camera). The miss left `mLoadPos=(0,0,0)`,
so `moveToLoadFromTitle` panned toward the origin. The correct tool sits at (1095,328,-13) = the
oracle settled camPos; `loadAt` now resolves to the oracle camTarget (1148.47,413.80,-1007.88).

**Verified:** settled camera pos/target/fovy now match the Dolphin oracle exactly; horizontal
framing aligns with the aurora replay of the oracle file-select fifo (per-object dx offset
50→2 px, residual 50→22). scratch/pndump/cam_fixed_sbs.png (replay | native). Remaining uniform
~9 px vertical is s16 pitch-clamp quantization + capture-moment, not the mirror bug.

Reference note: the earlier `fsel_dolphin_end.png` "oracle" is a DIFFERENT (wider) camera state
than the settled load camera — use the aurora replay of `fsel_try_7300.dff`, or a
`--pad-start-at` Dolphin settle (scratch/pndump/dolpan/d060.png), for matched-state comparison.

## 🟡 SHADOW — view-matrix fix landed; crisp oracle shadow still needs the Z-stencil port (b03117b3)

Root of "missing": `TMBindShadowManager::drawShadow` did `GXLoadPosMtxImm(j3dSys.getViewMtx())`,
but retail (0x8022f014) loads `graphics->mViewMtx` (+0xB4). At the `丸影複合型` dispatch point
j3dSys still holds the MIRROR pass's Y-reflected view, so the disc transformed by the wrong
matrix and never appeared in the main view. Fixed to `g->getUnkB4()`; SB_SHADOW_VIZ (force red +
Z-test off) confirms the disc now projects to Mario's feet.

STILL not oracle-faithful: this is the simplified flat-decal path — the disc Z-fights the
coplanar ground poly (the +0.1 lift is insufficient at ~950-unit distance) and does not
reproduce retail's Z-buffer-as-stencil two-pass shadow (the FUN_8022f014 volume path, a
documented simplification). The crisp dark oval needs that stencil `drawShadow` ported. Mario's
shadow is a COMPOSITE of several circle requests (a body one at ~(840,100,-1000) r85 + the
entryDrawShadow foot one at (950,…) r40) — `shadow_project` preserves X, the differing X values
are separate requests, not a bug.

## Tooling added
- SB_LOG=camlook (final C_MTXLookAt eye/up/target/fovy/aspect).
- SB_SHADOW_VIZ (force disc red + Z-off — separates placement from visibility).
- aurora draw-dump prj now includes cx/cy (proj[2]/proj[6]) for projection-skew diffs.
- SB_DUMP_FRAME_EVERY (pre-existing) — periodic dumps for pan-trajectory capture (note: in
  SB_TURBO presents accumulate far faster than paced game-frames, so the pan completes before
  present ~560; dump from present 0 to catch the transition).
