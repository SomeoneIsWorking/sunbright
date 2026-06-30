# File-select GEOMETRY gap is closed by SB_OWN_GXLIST, not by hand-wiring more draw passes (2026-06-30)

## TL;DR (value-verified)
The file-select SCENE-pass geometry divergence the prior handoff named as THE next task —
**scene verts oracle 22683 vs native 6531 (relΔ 0.71)** under the default hand-driven scene_drive
path — is an artifact of the hand-driven path being **structurally incomplete**. Switching the
native run to **`SB_OWN_GXLIST=1`** (which captures the REAL master GX perform-list render in
`TMarDirector::direct`, `mPerformListGX->perform(0xffffffff)`) closes it:

```
                       oracle    native    relΔ
hand-driven (default)  22683.0   6531.0    0.71   <- the "next task" gap
SB_OWN_GXLIST=1        22683.0   23118.0   0.02   <- CONVERGED
```

Light count stays 3-vs-3 under both. So the prior handoff's proposed approach (hand-wire a
`drive_group` for each missing global TDrawBufObj buffer: MapOpa/MapXlu/Sky-Xlu/manager) was the
**wrong path** — it re-implements by hand exactly what the master GX perform list already does
faithfully, and risks the drive_map-style z-fight duplication ([[fileselect-dither-drive-map-dup]]).

## Why the hand-driven path can never reach the full geometry
`scene_drive.cpp`'s default path drives `通常シーン->perform(0x8)`. But `MarDirectorSetupObjects.cpp`
only inserts THREE children into 通常シーン: `gpConductor`, `gpLightManager`, a `PERF Event Group`.
So `perform(0x8)` draws only the conductor's actors + the light-manager buffer (+ the hand-wired
`drive_sky`/`drive_chr`). The bulk of the scene — the map terrain/buildings (マップグループ → DrawBuf
MapOpa/MapXlu), the manager group, the player group — is drawn by the **master GX perform list**
(`TMarDirector::preEntry`, `MarDirectorPreEntry.cpp`) into the GLOBAL draw buffers, NOT by
`通常シーン->perform`. The hand-driven path never touches those, so it tops out at ~6.5k verts.

`SB_OWN_GXLIST` runs that real perform list (`mPerformListGX->perform(0xffffffff)`) and the J3DShape
draw taps land in the capture once-per-present (bracketed by `sb_boot_capture_begin/end_scene`,
MarDirectorDirect.cpp ~270). That is the faithful, complete render — 23118 verts, 88 batches.

## Status of SB_OWN_GXLIST as the default
NOT flipped to default yet. It is the geometry-complete FOUNDATION, but it still renders
**overbright/washed** (frame 327 mid-pan mean rgb ~190,194,215; full-screen-populated, not empty).
The hand-driven default has correct-ish lighting but only ¼ of the geometry — a dead end. So the
project's render foundation should move to SB_OWN_GXLIST; the remaining file-select work is the
overbright/lighting on that foundation, then flip the default.

Residual parity under SB_OWN_GXLIST (the NEXT divergences, in priority order):
- **overbright** — scene washes to ~white. Prime suspects: per-vertex lighting accumulates 3 white
  lights (`sb_light_vertex_color0`), and/or ambient. `amb` parity is whole-frame-last-seen
  (0.50 vs 1.00) — a known confound, not necessarily the cause. Own the ngx light model next.
- `batches~` 259 vs 121 and `on-screen v~` 22683 vs 476 are **not cross-comparable** (the GX-stream
  oracle emits no NDC, so its onscr==nverts trivially; native's low onscr is the vast distant
  sea/sky geometry beyond the frustum — confirmed by the full-screen NDC bbox and 100%-populated
  frame). parity_sweep already flags these as grouping/clip-confounded.

## Repro
`tools/render/fileselect_value_oracle.sh` (now runs the native side with SB_OWN_GXLIST=1).
Oracle jsonl: scratch/passes/fs_oracle.jsonl; native: scratch/passes/fs_native.jsonl.

## The overbright, now QUANTIFIED as a value (the next divergence's verification harness)
Two new tools turn the overbright into a number a fix must move:
- **`SB_FRAME_DUMP_SETTLE=1`** (sms_boot_present.cpp) — dump starts once the option camera has
  SETTLED (the file-select choice scene, settles ~present frame 564; gates on `sb_camera_view_settled`,
  mirroring `SB_BATCH_DBG=-1`). This is the sms-boot companion to `fileselect_oracle.sh`'s settled
  Dolphin-GX capture — both can now be compared at the SAME settled state. (Earlier handoff frames at
  ~327 were the intro PAN — misleading. The camera settles by ~564 present frames, not the "1690"
  LOGIC-frame figure.)
- **`tools/render/fileselect_overbright.py`** — per-channel + 4x4 per-region MEAN-RGB delta between a
  settled native frame and `scratch/oracle/fileselect_gx_oracle.png`.

Measured (SB_OWN_GXLIST, settled frame 575 vs Dolphin-GX oracle):
```
  channel   native   oracle   delta(N-O)   std N / O
    R        181.6    118.0     +63.6       84.8 / 80.0
    G        200.4    159.4     +41.0       62.9 / 61.8
    B        213.9    190.5     +23.4       68.3 / 71.6
  mean |delta| 42.7   std-preserving(additive)=True
```
The std MATCHES per channel (84.8≈80, 62.9≈61.8, 68.3≈71.6) → the overbright is an **additive,
std-preserving brightness offset** (~[+64,+41,+23], RED-dominant), NOT a per-material multiply and
NOT structural noise (geometry/textures are right). The 4x4 grid puts the worst overbright in the
upper sky / distant beach (top-center +140,+88,+45); the only region where native is DARKER is the
top-right (block/banner area). A std-preserving, sky-concentrated, additive, red-dominant offset
points at an over-contributing **additive layer** (sky-ray / sun-glow / a post pass), not the per-
vertex diffuse lighting (which the SB_J3D_DBG data shows is light0-only / CLAMP / register-ambient
with most materials lighting-OFF).

## AUTOMATED ATTRIBUTION (the tooling does the drilling — `fileselect_overbright_drill.sh`)
Rather than eyeball batch dumps, the harness ablates each blend class (new `SB_ABLATE_BM=src/dst`
flag in sms_boot_present.cpp) and re-measures the overbright delta. Ranked (lower = more overbright
removed):
```
  25.2  no_screen_1_3   (drop SCREEN blend  src=ONE dst=INVSRCCLR=1/3)  <- -17.5  the biggest culprit
  31.9  no_additive_4_1 (drop additive      src=SRCALPHA dst=ONE=4/1)   <- -10.8  second
  42.6  no_premul_1_5   (drop ONE/INVSRCALPHA)                          negligible
  42.7  baseline
  42.7  no_onealpha_1_1
  51.7  no_imm_overlay  (drop the 2D overlay)                          WORSE (overlay was helping)
  84.8  no_all_blend    (drop ALL blended batches)                     FAR WORSE
```
So the overbright is dominated by TWO sky layers: the **SCREEN-blend** base (1/3) and the **additive**
ray/glow (4/1). `no_all_blend` being far worse proves these layers EXIST in the oracle too (the scene
NEEDS them) — so the fix is to render them with the CORRECT (dimmer/coloured) raster, NOT to drop
them. Both render with full-white raster (rgb=1,1,1; the SCREEN base is untextured, ntex=0) per the
SB_BATCH_DBG dump — prime suspect = the capture sourcing matColor(white) where the material's COLOR0
is VERTEX-sourced (cc0 bit0=1 → a sky gradient), so a white quad screen-blends the scene toward white.
NEXT: trace those two materials' raster colour source (vertex vs register) + TEV, fix the white raster.
