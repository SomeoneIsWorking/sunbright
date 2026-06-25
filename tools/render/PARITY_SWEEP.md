# Pure-Dolphin vs sms-boot parity sweep

Find rendering divergences between the **oracle (pure Dolphin** — the real game under Dolphin's
JIT + GX) and the **PC-native engine (sms-boot)**, at the VALUE level. No pixel diffing.

There is ONE PC-native engine (sms-boot) and ONE oracle (pure Dolphin). The Dolphin-ngx renderer
is not a parity target.

## What is compared (and why)

Only **renderer-independent game state** — what both engines compute from the same J3D data, so a
faithful native engine must match the oracle exactly:

- **projection / viewport** — `GXSetProjection` / `GXSetViewport` args (exact game state).
- **lights** — count, position, colour; ambient + material-colour registers.
- **frame geometry aggregate** — on-screen vertex count, NDC AABB over the on-screen verts, and a
  position checksum. The batch/shape *grouping* differs between engines, but the projected
  on-screen geometry must match if both transform/skin faithfully. A skinning/pose/camera bug
  moves these.

We do NOT compare Dolphin's read-back `xfmem` / GP registers (async-lagged → not a valid oracle)
and we do NOT pixel-diff. The GX *call args* / J3D objects are the valid source on both sides.

## Emitters (both write the same JSONL `geom` schema, one line per frame)

- **sms-boot (native)** — `SB_PARITY_DUMP=path` (`native/render/sb_parity_dump.h`, from the present):
  ```
  SB_FRAME_DUMP=1 SB_FRAME_DUMP_START=240 SB_FRAME_DUMP_MAX=30 SB_PARITY_DUMP=scratch/frames/native.jsonl \
    setarch -R env SUNBRIGHT_DISC=scratch/disc/sms.iso SB_THP_FAST=1 SB_TURBO=1 ./build-native/sms-boot
  ```
- **pure Dolphin (oracle)** — `SUNBRIGHT_PARITY_DUMP=path` (emitted from `ngx_frame_publish` in
  `runtime/overrides/ngx_j3d_shape.cpp`, reading the captured J3D geometry). Run Dolphin-GX render
  with capture on (capture is diagnostic-only; it does not change the on-screen image):
  ```
  SDL_VIDEODRIVER=x11 SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_NGX_SHAPE=1 SUNBRIGHT_TURBO=1 \
    SUNBRIGHT_PARITY_DUMP=scratch/frames/oracle.jsonl ./build/sunbright
  ```

The engines number frames independently, so cross-engine `diff` either matches on shared frame
indices (when the windows overlap) or falls back to a **window summary** (median verts / on-screen
count / screen-XY extent) — convention-robust (Y-up/down and NDC-Z range don't matter).

Compared cross-engine: total verts, on-screen count (w>0 ∧ |x/w|,|y/w| ≤ 1 — Z excluded, its range
differs), screen-XY AABB. NOT compared cross-engine: projection form, lights, NDC-Z, pixels.

## Sweep — `tools/render/parity_sweep.py`

```
parity_sweep.py check dump.jsonl                 # invariants on one dump
parity_sweep.py diff  oracle.jsonl native.jsonl  # divergences: pure Dolphin vs sms-boot
```

- **check** — hard FAIL on: NaN/inf verts, empty scene, nothing on-screen (broken projection),
  an ON-SCREEN point-collapse, near-black / blown-white XFB. Off-screen point-collapse = warning.
  rc=1 on any hard fail ⇒ doubles as a regression gate. (`parity_run.sh` runs fastboot+check.)
- **diff** — matches frames by index; reports divergence in projType/proj/viewport, light
  count/ambient/material, and the frame geometry aggregate (on-screen count, NDC AABB, NaN). When
  both dumps share the same batch grouping (native A/B), also reports per-batch clip-AABB drift.
  Fails loudly if the dump windows don't overlap. (Demonstrated native A/B: `SB_NO_SKIN` flags
  Mario's batches `c539bdd263592117`.)

## Loop

```
# regenerate the native dump (one-command gate)
tools/render/parity_run.sh                                  # run sms-boot + check
# vs the oracle
parity_sweep.py diff oracle.jsonl scratch/frames/parity.jsonl
```
