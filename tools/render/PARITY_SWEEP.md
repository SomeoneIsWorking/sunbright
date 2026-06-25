# Dolphin-GX vs PC-native parity sweep

Tools to verify the PC-native renderer against the GameCube/Dolphin-GX behaviour across
**geometry, lighting, and the XFB / final image** — at the VALUE level (not by eyeballing an
overbright frame).

## Two tracks (this split is forced by a hard project rule)

Dolphin's CPU-side intermediate state (`xfmem`, GP registers) is **async-lagged and is NOT a
valid oracle** (see CLAUDE.md / memory `xfmem-not-cpu-oracle`). So:

- **VALUE track** — the native engine's own per-frame geometry+lighting+XFB-grid state, dumped
  as JSONL. Verified against the SPEC + self-consistency (`check`) and against another native
  run (`diff`), *not* against Dolphin's lagged state. This is the track that caught the
  per-vertex-skinning collapse and the weighted-envelope bug.
- **PIXEL track** — the only trustworthy comparison vs Dolphin-GX is the **rendered frame**.
  `image` does a per-region pixel diff of the native frame against a Dolphin-GX oracle frame of
  the same state, and refuses an all-black/empty frame so it can't report a number vs a dead oracle.

## Emit the value-track dump (native engine)

```
SB_PARITY_DUMP=scratch/frames/parity.jsonl  ... ./build-native/sms-boot
```
Writes one JSON line per **dumped** frame (pairs 1:1 with the `SB_FRAME_DUMP` PPMs), holding:
per-batch `{shaderKey, vcount, on-screen count, clip-space AABB (xyzw, pre-divide → always
finite), z/blend state, ntex, checksum, NaN/inf count}`, the light state (count / pos / colour),
ambient + material-colour registers, and a 4×4 XFB region grid + overall brightness.
Implemented in `native/render/sb_parity_dump.h` (`sb_parity_emit`), called from the present.

## Run the sweep — `tools/render/parity_sweep.py`

```
parity_sweep.py check  parity.jsonl              # invariants on one native run
parity_sweep.py diff   a.jsonl b.jsonl           # A/B between two native runs (regression localiser)
parity_sweep.py image  native.ppm oracle.ppm     # per-region pixel diff vs a Dolphin-GX frame
```

- **check** — hard FAIL on: NaN/inf verts, empty scene, nothing on-screen (broken projection),
  an ON-SCREEN point-collapse, near-black / blown-white XFB. Reports off-screen collapses as
  **warnings** (real degeneracies — often a parked/hidden model — but non-blocking). rc=1 on any
  hard fail, so it doubles as a CI/regression gate.
- **diff** — matches frames by index and batches by order; reports lighting / XFB-brightness /
  on-screen-count / clip-AABB drift above thresholds, localising *which batch* a change moved.
  (Demonstrated: toggling `SB_NO_SKIN` flags exactly Mario's batches `c539bdd263592117`.)
- **image** — overall + per-region (`sky / mid / floor / center / hud_top / hud_bot`) mean |Δ|.
  Pair a native PPM with a Dolphin-GX oracle PPM of the same fastboot/save state. (For the
  main `sunbright` build, the Dolphin-GX oracle frame = a `SUNBRIGHT_NGX_PRESENT=0` run; see
  `tools/render/ab_oracle.sh`.)

## Typical loop

```
# value track — did my renderer change regress geometry/lighting anywhere?
SB_PARITY_DUMP=scratch/frames/before.jsonl ... ./build-native/sms-boot   # baseline
#  ... make the change, rebuild ...
SB_PARITY_DUMP=scratch/frames/after.jsonl  ... ./build-native/sms-boot
parity_sweep.py check after.jsonl            # no new hard fails?
parity_sweep.py diff  before.jsonl after.jsonl   # only the batches I intended moved?

# pixel track — does the final image match Dolphin-GX?
parity_sweep.py image native.ppm oracle.ppm
```
