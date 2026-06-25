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

## Emitters (both write the same JSONL schema, one line per dumped frame)

- **sms-boot** — `SB_PARITY_DUMP=path` (`native/render/sb_parity_dump.h`, from the present).
- **pure Dolphin** — `tools/render/dolphin_j3d_probe.py` *(oracle side; reads the same J3D state
  from the running main build — WIP)*.

Both fastboot the same Delfino state; dumps pair by frame index (deterministic fastboot ⇒ matched
game state at the same frame number).

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
