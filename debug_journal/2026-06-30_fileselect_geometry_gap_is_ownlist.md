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
